/*
 * sys_stats.c - stats_task, see sys_stats.h.
 *
 * Idle measurement (implementation.md "Power management"):
 *   - ulTaskGetIdleRunTimePercentForCore(core): the spec'd FreeRTOS API (idle % since
 *     boot, per core, ESP-IDF's per-core variant of ulTaskGetIdleRunTimePercent()).
 *   - The same counters (ulTaskGetIdleRunTimeCounterForCore) differenced over the last
 *     interval, which is what you want when comparing DETECT vs STREAM periods.
 *   The run-time clock is esp_timer (microseconds), 64-bit counters (sdkconfig.defaults).
 */
#include "sys_stats.h"

#include <inttypes.h>

#include "app_config.h"
#include "audio_ingest.h"
#include "dsp_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nn_task.h"
#include "pipeline.h"

static const char *TAG = "stats";

#define CPU_LOAD_BUDGET_PCT 10.0f

static uint32_t stack_free(TaskHandle_t t)
{
    return (t != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0u; /* bytes in ESP-IDF */
}

void stats_task(void *arg)
{
    (void)arg;
    pipeline_wait_start();
    const pipeline_tasks_t *tasks = pipeline_tasks();

    int64_t last_us = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE last_idle[portNUM_PROCESSORS];
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        last_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    ESP_LOGI(TAG, "stats_task on core %d, report every %u hops", xPortGetCoreID(),
             (unsigned)SIH_STATS_PERIOD_HOPS);

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const int64_t now = esp_timer_get_time();
        const float span_us = (float)(now - last_us);
        last_us = now;

        float idle_int[portNUM_PROCESSORS];
        uint32_t idle_boot[portNUM_PROCESSORS];
        for (int c = 0; c < portNUM_PROCESSORS; c++) {
            const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(c);
            idle_int[c] = (span_us > 0.0f) ? 100.0f * (float)(idle - last_idle[c]) / span_us : 0.0f;
            if (idle_int[c] > 100.0f) {
                idle_int[c] = 100.0f; /* counters update on context switch: tiny overshoot */
            }
            last_idle[c] = idle;
            idle_boot[c] = (uint32_t)ulTaskGetIdleRunTimePercentForCore(c);
        }

        ingest_stats_t in;
        audio_ingest_get_stats(&in);
        dsp_timing_t dt;
        dsp_task_take_timing(&dt);
        nn_stats_t nn;
        nn_task_take_stats(&nn);

        const float load0 = 100.0f - idle_int[0];
        const float load1 = 100.0f - idle_int[1];
        const bool within = load0 < CPU_LOAD_BUDGET_PCT && load1 < CPU_LOAD_BUDGET_PCT;

        ESP_LOGI(TAG, "---- t=%.1fs mode=%s ----", (double)now / 1e6, pipeline_mode_name(pipeline_mode()));
        ESP_LOGI(TAG, "CPU  idle%% interval C0 %.1f C1 %.1f | since boot C0 %" PRIu32 " C1 %" PRIu32
                 " | load C0 %.1f%% C1 %.1f%% (budget <%.0f%%: %s)",
                 (double)idle_int[0], (double)idle_int[1], idle_boot[0], idle_boot[1], (double)load0,
                 (double)load1, (double)CPU_LOAD_BUDGET_PCT, within ? "OK" : "OVER");
        ESP_LOGI(TAG, "I2S  hops %" PRIu32 " (+%" PRIu32 ") dma_q_ovf %" PRIu32 " rd_err %" PRIu32
                 " empty_wakes %" PRIu32 " | level peak %.1f dBFS rms %.1f dBFS dc %+.4f",
                 in.hops_total, in.hops_interval, in.dma_queue_overflows, in.read_errors, in.empty_wakes,
                 (double)in.peak_dbfs, (double)in.rms_dbfs, (double)in.dc_offset);
        ESP_LOGI(TAG, "DSP  hops +%" PRIu32 " wakes +%" PRIu32 " period min/avg/max %" PRIu32 "/%" PRIu32 "/%" PRIu32
                 " us | proc avg/max %" PRIu32 "/%" PRIu32 " us | max hops/wake %" PRIu32 " | wrap windows +%" PRIu32
                 " | rd_err %" PRIu32 " resync %" PRIu32,
                 dt.hops, dt.wakes, dt.period_min_us, dt.period_avg_us, dt.period_max_us, dt.proc_avg_us,
                 dt.proc_max_us, dt.max_hops_per_wake, dt.wrap_windows, dt.read_errors, dt.resyncs);
        ESP_LOGI(TAG, "GATE tier1 ran DSP +%" PRIu32 " gated off +%" PRIu32 " (DSP skipped on %.0f%% of hops)",
                 dt.dsp_ran, dt.dsp_gated,
                 (double)(dt.dsp_ran + dt.dsp_gated ? 100.0f * (float)dt.dsp_gated / (float)(dt.dsp_ran + dt.dsp_gated) : 0.0f));
        ESP_LOGI(TAG, "NN   snapshots +%" PRIu32 " skipped(busy) +%" PRIu32 " inferences +%" PRIu32 " errors +%" PRIu32
                 " max conf %.2f | WAKE_CONFIRMED total %" PRIu32,
                 dt.nn_notifies, dt.nn_skipped, nn.invocations, nn.invoke_errors, (double)nn.conf_max,
                 nn.triggers_total);
        ESP_LOGI(TAG, "MEM  internal heap free %u B (min ever %u B of %u B) | stack free: ingest %" PRIu32
                 " dsp %" PRIu32 " nn %" PRIu32 " net %" PRIu32 " stats %" PRIu32 " B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL), stack_free(tasks->ingest),
                 stack_free(tasks->dsp), stack_free(tasks->nn), stack_free(tasks->net), stack_free(tasks->stats));
    }
}
