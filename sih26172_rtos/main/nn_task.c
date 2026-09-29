/*
 * nn_task.c - nn_task: feature-matrix snapshot from dsp_task -> TFLite Micro model
 * (kws_nn.h, the NN team's model + glue) -> WAKE_CONFIRMED.
 *
 *   - wakes on NN_EVT_INFER from dsp_task, never polls
 *   - quantizes the snapshot into the model input, releases it, then runs the model
 *     (releasing before Invoke lets dsp_task publish the next snapshot meanwhile)
 *   - parks while the pipeline is not in DETECT mode (DESIGN CHOICE in pipeline.c),
 *     dropping any stale snapshot; resets the NN bouncer when detection resumes
 *   - emits WAKE_CONFIRMED via pipeline_emit_wake_confirmed() with the timestamp taken
 *     right before the call and no logging in between (T1 latency, wake_event.h)
 *   - the BOOT button remains as a manual trigger for bring-up of the streaming path
 */
#include "nn_task.h"

#include <stdbool.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kws_nn.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipeline.h"
#include "wake_event.h"

static const char *TAG = "nn";

static TaskHandle_t s_self;
static volatile bool s_button_armed; /* one trigger per arm; re-armed when detection resumes */

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static nn_stats_t s_stats;

void nn_task_take_stats(nn_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    s_stats.invocations = 0;
    s_stats.invoke_errors = 0;
    s_stats.conf_max = 0.0f;
    taskEXIT_CRITICAL(&s_stats_lock);
}

#if CONFIG_SIH_TRIGGER_BUTTON_ENABLE
static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    /* Disarming here doubles as debounce: contact bounce after the first edge is ignored
     * until detection resumes after the stream + cooldown. */
    if (!s_button_armed) {
        return;
    }
    s_button_armed = false;
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_self, NN_EVT_BUTTON, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_SIH_TRIGGER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE: already installed */
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)CONFIG_SIH_TRIGGER_BUTTON_GPIO, button_isr, NULL));
    s_button_armed = true;
    ESP_LOGI(TAG, "manual trigger: press the button on GPIO%d to fire WAKE_CONFIRMED",
             CONFIG_SIH_TRIGGER_BUTTON_GPIO);
}
#endif

void nn_task(void *arg)
{
    (void)arg;
    pipeline_wait_start();
    s_self = xTaskGetCurrentTaskHandle();

#if CONFIG_SIH_TRIGGER_BUTTON_ENABLE
    button_init();
#endif
    /* Builds the interpreter in the static tensor arena and checks the model against the
     * DSP output shape (100 x 40). On failure the task keeps running so the button trigger
     * and the streaming path still work; the log says why there is no detection. */
    const bool model_ok = kws_nn_init();
    if (!model_ok) {
        ESP_LOGE(TAG, "NN model NOT available: wake-word detection disabled");
    }
    ESP_LOGI(TAG, "nn_task running on core %d (fed by dsp_task when the DSP gates pass, ~every %d hops)",
             xPortGetCoreID(), KWS_FE_INVOKE_EVERY_HOPS);

    for (;;) {
        uint32_t bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if (pipeline_mode() != PIPE_MODE_DETECT) {
            /* Parked while streaming / cooling down: the DSP/NN pipeline is bypassed.
             * Anything that arrives now is stale. Free the snapshot so dsp_task is never
             * left waiting on a snapshot nobody will read. */
            pipeline_snapshot_release();
            continue;
        }

        if (bits & NN_EVT_RESUME) {
            pipeline_snapshot_release();
            kws_nn_reset(); /* NN bouncer history belongs to the old session */
            s_button_armed = true;
        }

        bool fire = false;
        float confidence = 1.0f;
        if ((bits & NN_EVT_INFER) && model_ok) {
            const kws_fe_matrix_t *snap = pipeline_snapshot_get();
            if (snap != NULL) {
                kws_nn_load_input(snap);
                pipeline_snapshot_release();

                kws_nn_result_t res;
                const bool ok = kws_nn_run(&res);
                taskENTER_CRITICAL(&s_stats_lock);
                s_stats.invocations++;
                if (!ok) {
                    s_stats.invoke_errors++;
                } else if (res.confidence > s_stats.conf_max) {
                    s_stats.conf_max = res.confidence;
                }
                taskEXIT_CRITICAL(&s_stats_lock);
                if (ok && res.triggered) {
                    fire = true;
                    confidence = res.confidence;
                }
            }
        } else if (bits & NN_EVT_INFER) {
            pipeline_snapshot_release(); /* no model: do not block dsp_task */
        }
        if (bits & NN_EVT_BUTTON) {
            fire = true;
        }

        if (fire) {
            s_button_armed = false;
            /* T1 checkpoint. Nothing but the handoff between these two lines. */
            const wake_confirmed_t evt = {
                .confidence = confidence,
                .timestamp_us = esp_timer_get_time(),
            };
            pipeline_emit_wake_confirmed(&evt);

            taskENTER_CRITICAL(&s_stats_lock);
            s_stats.triggers_total++;
            taskEXIT_CRITICAL(&s_stats_lock);
            /* No logging here: network_task (same core, lower priority) must run now.
             * It logs the event. */
        }
    }
}
