/*
 * main.c - SIH26172 RTOS / audio ingestion / networking subsystem, ESP32-S3.
 *
 * app_main():
 *   1. power management: DFS + automatic light sleep, tickless idle (sdkconfig.defaults)
 *   2. pipeline_init(): ring buffer, WAKE_CONFIRMED mailbox, start barrier (all static)
 *   3. create every task with xTaskCreateStaticPinnedToCore (static stacks and TCBs,
 *      the ESP-IDF call that is both "xTaskCreateStatic" and "xTaskCreatePinnedToCore")
 *   4. print the static RAM footprint next to the NN team's Tensor Arena
 *   5. release the start barrier and return (the main task is deleted by ESP-IDF)
 *
 * Memory rule: nothing in this project calls malloc/new. ESP-IDF drivers and stacks we
 * use (I2S driver, GPIO ISR service, Wi-Fi, lwIP, mbedTLS, esp_websocket_client, event
 * loop) allocate internally; that heap use is reported live by stats_task ("MEM").
 */
#include <inttypes.h>
#include <stdio.h>

#include "app_config.h"
#include "audio_ingest.h"
#include "dsp_task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_stream.h"
#include "kws_nn.h"
#include "nn_task.h"
#include "pipeline.h"
#include "sdkconfig.h"
#include "sys_stats.h"

static const char *TAG = "main";

_Static_assert(portNUM_PROCESSORS == 2, "This design pins tasks to both cores of the ESP32-S3");
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS must be enabled (see sdkconfig.defaults)"
#endif

/* ------------------------------------------------------------------ static task memory */
static StackType_t s_stack_ingest[SIH_STACK_INGEST];
static StackType_t s_stack_dsp[SIH_STACK_DSP];
static StackType_t s_stack_nn[SIH_STACK_NN];
static StackType_t s_stack_net[SIH_STACK_NET];
static StaticTask_t s_tcb_ingest;
static StaticTask_t s_tcb_dsp;
static StaticTask_t s_tcb_nn;
static StaticTask_t s_tcb_net;
#if CONFIG_SIH_STATS_ENABLE
static StackType_t s_stack_stats[SIH_STACK_STATS];
static StaticTask_t s_tcb_stats;
#endif

static void power_init(void)
{
#if CONFIG_PM_ENABLE
    /* DESIGN NOTE: automatic light sleep is enabled as the spec asks, but while I2S is
     * capturing (always, for an always-listening wake word) the I2S driver holds an
     * ESP_PM_APB_FREQ_MAX lock, so the chip will not actually enter light sleep and the
     * CPU floor is 80 MHz. Tickless idle + DFS still apply; the <10% CPU budget is
     * verified by stats_task, not assumed. */
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40, /* XTAL */
        .light_sleep_enable = true,
    };
    const esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        const char *tickless = "on";
#else
        const char *tickless = "OFF (enable CONFIG_FREERTOS_USE_TICKLESS_IDLE)";
#endif
        ESP_LOGI(TAG, "power management: DFS %d-%d MHz, auto light sleep on, tickless idle %s",
                 pm.min_freq_mhz, pm.max_freq_mhz, tickless);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE is off: no DFS / light sleep / tickless idle");
#endif
}

static TaskHandle_t create_task(TaskFunction_t fn, const char *name, uint32_t stack_bytes, StackType_t *stack,
                                StaticTask_t *tcb, UBaseType_t prio, BaseType_t core)
{
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(fn, name, stack_bytes, NULL, prio, stack, tcb, core);
    configASSERT(h != NULL);
    return h;
}

static void report_footprint(void)
{
    uint32_t stacks = SIH_STACK_INGEST + SIH_STACK_DSP + SIH_STACK_NN + SIH_STACK_NET;
    uint32_t tcbs = 4u * (uint32_t)sizeof(StaticTask_t);
#if CONFIG_SIH_STATS_ENABLE
    stacks += SIH_STACK_STATS;
    tcbs += (uint32_t)sizeof(StaticTask_t);
#endif
    const uint32_t ring = (uint32_t)pipeline_static_bytes();
    const uint32_t dsp = (uint32_t)dsp_task_state_size();
    const uint32_t ingest = audio_ingest_static_bytes();
    const uint32_t net = net_stream_static_bytes();
    const uint32_t ours = ring + dsp + ingest + net + stacks + tcbs;
    const uint32_t arena = (uint32_t)kws_nn_arena_bytes();
    const uint32_t budget = (uint32_t)CONFIG_SIH_RAM_BUDGET_BYTES;

    ESP_LOGI(TAG, "==== static RAM footprint (this subsystem) ====");
    ESP_LOGI(TAG, "  ring + NN snapshot + mailbox    : %6" PRIu32 " B  (%u samples = %u ms Q15.16)", ring,
             (unsigned)SIH_RING_CAPACITY, (unsigned)(SIH_RING_CAPACITY / SIH_SAMPLES_PER_MS));
    ESP_LOGI(TAG, "  DSP task + frontend + work bufs : %6" PRIu32 " B", dsp);
    ESP_LOGI(TAG, "  ingest buffers                  : %6" PRIu32 " B", ingest);
    ESP_LOGI(TAG, "  streaming backlog + session     : %6" PRIu32 " B  (%d ms PCM16)", net,
             CONFIG_SIH_STREAM_BACKLOG_MS);
    ESP_LOGI(TAG, "  task stacks + TCBs              : %6" PRIu32 " B", stacks + tcbs);
    ESP_LOGI(TAG, "  subsystem total                 : %6" PRIu32 " B", ours);
    ESP_LOGI(TAG, "  NN Tensor Arena (NN team)       : %6" PRIu32 " B", arena);
    ESP_LOGI(TAG, "  combined                        : %6" PRIu32 " B of %" PRIu32 " B budget (%s)", ours + arena,
             budget, (ours + arena) <= budget ? "OK" : "OVER");
    ESP_LOGI(TAG, "  (+ ESP-IDF internal heap for Wi-Fi/lwIP/TLS/drivers: see stats 'MEM' lines)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "SIH26172 RTOS/audio/network subsystem starting on %d cores", portNUM_PROCESSORS);
    power_init();
    pipeline_init();

    pipeline_tasks_t t = {0};
    /* Consumers first, so every notification target exists before audio starts. All
     * tasks block on the start barrier until pipeline_start(). */
#if CONFIG_SIH_STATS_ENABLE
    t.stats = create_task(stats_task, "stats_task", SIH_STACK_STATS, s_stack_stats, &s_tcb_stats, SIH_PRIO_STATS,
                          SIH_CORE_STATS);
#endif
    t.net = create_task(network_task, "network_task", SIH_STACK_NET, s_stack_net, &s_tcb_net, SIH_PRIO_NET,
                        SIH_CORE_NET);
    t.nn = create_task(nn_task, "nn_task", SIH_STACK_NN, s_stack_nn, &s_tcb_nn, SIH_PRIO_NN, SIH_CORE_NN);
    t.dsp = create_task(dsp_task, "dsp_task", SIH_STACK_DSP, s_stack_dsp, &s_tcb_dsp, SIH_PRIO_DSP, SIH_CORE_DSP);
    t.ingest = create_task(audio_ingest_task, "audio_ingest", SIH_STACK_INGEST, s_stack_ingest, &s_tcb_ingest,
                           SIH_PRIO_INGEST, SIH_CORE_INGEST);

    report_footprint();
    pipeline_start(&t);
    ESP_LOGI(TAG, "all tasks released (ingest C%d/P%d, dsp C%d/P%d, nn C%d/P%d, net C%d/P%d)", SIH_CORE_INGEST,
             SIH_PRIO_INGEST, SIH_CORE_DSP, SIH_PRIO_DSP, SIH_CORE_NN, SIH_PRIO_NN, SIH_CORE_NET, SIH_PRIO_NET);
}
