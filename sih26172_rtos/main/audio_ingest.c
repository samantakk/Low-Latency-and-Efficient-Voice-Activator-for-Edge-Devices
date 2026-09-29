/*
 * audio_ingest.c - audio_ingest_task: I2S RX + DMA -> Q15.16 -> ring buffer.
 *
 * Event chain (no polling, no timers):
 *   I2S DMA finishes one 160-sample buffer (10 ms)
 *     -> on_recv_isr(): vTaskNotifyGiveFromISR(audio_ingest_task)
 *     -> audio_ingest_task wakes, pulls every completed DMA buffer with a zero timeout,
 *        converts each 32-bit I2S word to Q15.16, writes the hop to the ring buffer,
 *        then notifies exactly one consumer depending on the pipeline mode:
 *          DETECT   -> dsp_task      (xTaskNotifyGive)
 *          STREAM   -> network_task  (NET_EVT_AUDIO)
 *          COOLDOWN -> nobody (the ring keeps filling so DSP history stays valid)
 *     -> every CONFIG_SIH_STATS_PERIOD_HOPS hops, publishes level stats and wakes stats_task.
 */
#include "audio_ingest.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "audio_format.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipeline.h"
#include "sdkconfig.h"

static const char *TAG = "ingest";

#if CONFIG_SIH_MIC_SLOT_RIGHT
#define SIH_MIC_SLOT_MASK I2S_STD_SLOT_RIGHT
#define SIH_MIC_SLOT_NAME "right"
#else
#define SIH_MIC_SLOT_MASK I2S_STD_SLOT_LEFT
#define SIH_MIC_SLOT_NAME "left"
#endif

static i2s_chan_handle_t s_rx;
static TaskHandle_t s_self;                    /* read by the ISR */
static volatile uint32_t s_dma_q_overflows;    /* written by the ISR only */

static uint32_t s_raw[SIH_I2S_DMA_FRAMES];     /* one DMA buffer, copied out by the driver */
static q15_16_t s_hop[SIH_HOP_SAMPLES];        /* converted hop, written to the ring */

/* Level accumulators for the current stats interval (task-local use only). */
typedef struct {
    int64_t sum;
    int64_t sum_sq;
    int32_t peak;
    uint32_t samples;
    uint32_t hops;
} level_acc_t;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static ingest_stats_t s_stats_pub;

/* ------------------------------------------------------------------ ISR callbacks
 * Run in the I2S DMA interrupt (IRAM, CONFIG_I2S_ISR_IRAM_SAFE=y). Keep them tiny. */
static bool IRAM_ATTR on_recv_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_self, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_recv_q_ovf_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    s_dma_q_overflows++; /* oldest DMA buffer was dropped by the driver */
    return false;
}

/* ------------------------------------------------------------------ helpers */
static float to_dbfs(float amplitude)
{
    return (amplitude > 1e-6f) ? 20.0f * log10f(amplitude) : -120.0f;
}

static void i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_SIH_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SIH_I2S_DMA_FRAMES; /* one DMA buffer == one 10 ms hop */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SIH_SAMPLE_RATE_HZ),
        /* 32-bit words, mono. The mic's 18/24-bit sample is MSB-aligned inside. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)CONFIG_SIH_I2S_BCLK_GPIO,
            .ws = (gpio_num_t)CONFIG_SIH_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)CONFIG_SIH_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = SIH_MIC_SLOT_MASK;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));

    const i2s_event_callbacks_t cbs = {
        .on_recv = on_recv_isr,
        .on_recv_q_ovf = on_recv_q_ovf_isr,
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "I2S RX started: %u Hz, 32-bit mono (%s slot), %u valid bits, "
             "DMA %d x %u frames (%u bytes) = one hop per interrupt, pins BCLK=%d WS=%d DIN=%d",
             (unsigned)SIH_SAMPLE_RATE_HZ, SIH_MIC_SLOT_NAME,
             (unsigned)CONFIG_SIH_MIC_SAMPLE_BITS, CONFIG_SIH_I2S_DMA_DESC_NUM,
             (unsigned)SIH_I2S_DMA_FRAMES, (unsigned)SIH_I2S_DMA_BUF_BYTES,
             CONFIG_SIH_I2S_BCLK_GPIO, CONFIG_SIH_I2S_WS_GPIO, CONFIG_SIH_I2S_DIN_GPIO);
}

static void publish_stats(level_acc_t *acc, uint32_t hops_total, uint32_t read_errors,
                          uint32_t empty_wakes, uint32_t write_pos)
{
    ingest_stats_t s = {0};
    s.hops_total = hops_total;
    s.hops_interval = acc->hops;
    s.dma_queue_overflows = s_dma_q_overflows;
    s.read_errors = read_errors;
    s.empty_wakes = empty_wakes;
    s.write_pos = write_pos;
    if (acc->samples > 0) {
        const float n = (float)acc->samples;
        const float mean = (float)acc->sum / n;              /* Q15.16 units */
        float var = (float)acc->sum_sq / n - mean * mean;    /* Q15.16 units^2 */
        if (var < 0.0f) {
            var = 0.0f;
        }
        s.dc_offset = mean / (float)Q15_16_ONE;
        s.rms_dbfs = to_dbfs(sqrtf(var) / (float)Q15_16_ONE);
        s.peak_dbfs = to_dbfs((float)acc->peak / (float)Q15_16_ONE);
    } else {
        s.rms_dbfs = -120.0f;
        s.peak_dbfs = -120.0f;
    }
    taskENTER_CRITICAL(&s_stats_lock);
    s_stats_pub = s;
    taskEXIT_CRITICAL(&s_stats_lock);
    memset(acc, 0, sizeof(*acc));
}

void audio_ingest_get_stats(ingest_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_lock);
    *out = s_stats_pub;
    taskEXIT_CRITICAL(&s_stats_lock);
}

uint32_t audio_ingest_static_bytes(void)
{
    return (uint32_t)(sizeof(s_raw) + sizeof(s_hop) + sizeof(s_stats_pub));
}

/* ------------------------------------------------------------------ task */
void audio_ingest_task(void *arg)
{
    (void)arg;
    pipeline_wait_start();

    s_self = xTaskGetCurrentTaskHandle();
    const pipeline_tasks_t *tasks = pipeline_tasks();
    ring_buffer_t *ring = pipeline_ring();

    /* Created from this task (pinned to core 0) so the DMA interrupt is allocated on
     * core 0 too: ISR -> task wakeup never crosses cores. */
    i2s_start();

    level_acc_t acc = {0};
    uint32_t hops_total = 0;
    uint32_t read_errors = 0;
    uint32_t empty_wakes = 0;
    uint32_t hops_to_stats = SIH_STATS_PERIOD_HOPS;

    for (;;) {
        /* Blocks with zero CPU until the DMA-complete ISR gives the notification. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool got_any = false;
        for (;;) {
            size_t bytes = 0;
            /* Zero timeout: take whatever complete DMA buffers are queued, never wait. */
            const esp_err_t err = i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &bytes, 0);
            if (err == ESP_ERR_TIMEOUT && bytes == 0) {
                break; /* no more complete hops */
            }
            if (err != ESP_OK || bytes != sizeof(s_raw)) {
                read_errors++;
                break;
            }
            got_any = true;

            /* 32-bit I2S word -> normalized Q15.16 (interface #1). */
            int32_t peak = acc.peak;
            int64_t sum = 0;
            int64_t sum_sq = 0;
            for (uint32_t i = 0; i < SIH_HOP_SAMPLES; i++) {
                const q15_16_t q = i2s_word_to_q15_16(s_raw[i], CONFIG_SIH_MIC_SAMPLE_BITS);
                s_hop[i] = q;
                const int32_t mag = (q < 0) ? -q : q;
                if (mag > peak) {
                    peak = mag;
                }
                sum += q;
                sum_sq += (int64_t)q * q;
            }
            acc.peak = peak;
            acc.sum += sum;
            acc.sum_sq += sum_sq;
            acc.samples += SIH_HOP_SAMPLES;
            acc.hops++;

            rb_write(ring, s_hop, SIH_HOP_SAMPLES);
            hops_total++;

            switch (pipeline_mode()) {
            case PIPE_MODE_DETECT:
                xTaskNotifyGive(tasks->dsp);
                break;
            case PIPE_MODE_STREAM:
                xTaskNotify(tasks->net, NET_EVT_AUDIO, eSetBits);
                break;
            case PIPE_MODE_COOLDOWN:
            default:
                break;
            }

            /* SIH_STATS_PERIOD_HOPS is 0 when diagnostics are disabled (branch removed). */
            if (SIH_STATS_PERIOD_HOPS != 0u && --hops_to_stats == 0u) {
                hops_to_stats = SIH_STATS_PERIOD_HOPS;
                publish_stats(&acc, hops_total, read_errors, empty_wakes, rb_write_pos(ring));
                if (tasks->stats != NULL) {
                    xTaskNotifyGive(tasks->stats);
                }
            }
        }
        if (!got_any) {
            empty_wakes++;
        }
    }
}
