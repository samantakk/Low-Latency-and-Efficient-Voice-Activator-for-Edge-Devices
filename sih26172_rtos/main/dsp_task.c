/*
 * dsp_task.c - dsp_task: ring buffer -> DSP team's C++ frontend (via kws_fe.h).
 *
 * RTOS side (this file):
 *   - wake-up per hop from audio_ingest_task (counting notification, catches up if several
 *     hops arrive while it was delayed)
 *   - windowed reads from the ring buffer: 25 ms window ending at each 10 ms hop
 *   - one kws_fe_process_hop() call per hop. The frontend does tier-1 gate -> FFT/Mel/PCEN
 *     -> tier-2 gate -> feature matrix -> "invoke the NN?" decision, including the
 *     every-5-hops / any-hop-passed-both-tiers cadence. This file does NOT count hops or
 *     gate anything itself (it used to, with an ungated CONFIG cadence that contradicted
 *     the DSP team's design).
 *   - on invoke_nn: copy the feature matrix into the shared snapshot and notify nn_task
 *     (the live matrix stays owned by this task; see pipeline.h)
 *   - parking while the pipeline is not in DETECT mode, resync on resume, and a
 *     KWS_FE_CONTEXT_HOPS-hop warm-up before snapshots go to nn_task again
 *
 * The frontend state is a file-static inside kws_fe.cpp, touched only from here.
 */
#include "dsp_task.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kws_fe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipeline.h"
#include "ring_buffer.h"

static const char *TAG = "dsp";

/*
 * Task-side bookkeeping owned exclusively by dsp_task. The DSP state proper (PCEN, VAD tiers,
 * feature matrix, hop counter) lives in kws_fe.cpp and is likewise touched only by this task.
 */
typedef struct {
    /* ring buffer read position: absolute position one past the last sample of the
     * next hop to process (the window is [next_hop_end - 400, next_hop_end)) */
    uint32_t next_hop_end;
    uint32_t detect_epoch;   /* pipeline_detect_epoch() when we last ran in DETECT mode */
    uint32_t warmup_hops;    /* after resume: hops to run before nn_task is fed again */

    /* scratch for the current window (kept here, not on the stack). The frontend windows
     * it IN PLACE, which is why it is a copy and not a pointer into the ring. */
    q15_16_t window[SIH_WINDOW_SAMPLES];
} dsp_state_t;

/* ------------------------------------------------------------------ timing stats */
typedef struct {
    uint32_t wakes;
    uint32_t hops;
    uint32_t period_n;
    uint64_t period_sum_us;
    uint32_t period_min_us;
    uint32_t period_max_us;
    uint64_t proc_sum_us;
    uint32_t proc_max_us;
    uint32_t max_hops_per_wake;
    uint32_t wrap_windows;
    uint32_t read_errors;
    uint32_t resyncs;
    uint32_t nn_notifies;
    uint32_t nn_skipped;
    uint32_t dsp_ran;
    uint32_t dsp_gated;
} timing_acc_t;

static portMUX_TYPE s_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static timing_acc_t s_timing = {.period_min_us = UINT32_MAX};

void dsp_task_take_timing(dsp_timing_t *out)
{
    timing_acc_t t;
    taskENTER_CRITICAL(&s_timing_lock);
    t = s_timing;
    memset(&s_timing, 0, sizeof(s_timing));
    s_timing.period_min_us = UINT32_MAX;
    taskEXIT_CRITICAL(&s_timing_lock);

    memset(out, 0, sizeof(*out));
    out->wakes = t.wakes;
    out->hops = t.hops;
    out->period_min_us = (t.period_n > 0) ? t.period_min_us : 0;
    out->period_max_us = t.period_max_us;
    out->period_avg_us = (t.period_n > 0) ? (uint32_t)(t.period_sum_us / t.period_n) : 0;
    out->proc_avg_us = (t.wakes > 0) ? (uint32_t)(t.proc_sum_us / t.wakes) : 0;
    out->proc_max_us = t.proc_max_us;
    out->max_hops_per_wake = t.max_hops_per_wake;
    out->wrap_windows = t.wrap_windows;
    out->read_errors = t.read_errors;
    out->resyncs = t.resyncs;
    out->nn_notifies = t.nn_notifies;
    out->nn_skipped = t.nn_skipped;
    out->dsp_ran = t.dsp_ran;
    out->dsp_gated = t.dsp_gated;
}

size_t dsp_task_state_size(void)
{
    return sizeof(dsp_state_t) + kws_fe_state_bytes();
}

/* ------------------------------------------------------------------ task */
void dsp_task(void *arg)
{
    (void)arg;
    /* Static local: lives in .bss, persists across every hop, invisible to other files. */
    static dsp_state_t st;

    pipeline_wait_start();
        const ring_buffer_t *ring = pipeline_ring();

    st.next_hop_end = rb_write_pos(ring) + SIH_HOP_SAMPLES; /* first complete hop from now */
    st.detect_epoch = pipeline_detect_epoch();
    st.warmup_hops = 0;

    kws_fe_init();

    int64_t last_wake_us = 0;
    ESP_LOGI(TAG, "dsp_task running on core %d: %u-sample window every %u-sample hop, "
             "state %u B (task %u + frontend %u)", xPortGetCoreID(), (unsigned)SIH_WINDOW_SAMPLES,
             (unsigned)SIH_HOP_SAMPLES, (unsigned)dsp_task_state_size(), (unsigned)sizeof(dsp_state_t),
             (unsigned)kws_fe_state_bytes());

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t t_wake = esp_timer_get_time();

        if (pipeline_mode() != PIPE_MODE_DETECT) {
            /* Streaming or cooldown: park. ingest does not notify us in these modes, so
             * we stay blocked (zero CPU) until network_task's resume notification. */
            continue;
        }

        uint32_t wp = rb_write_pos(ring);
        uint32_t resyncs = 0;
        const uint32_t epoch = pipeline_detect_epoch();
        if (epoch != st.detect_epoch) {
            /* Resumed after streaming + cooldown. The frontend state (PCEN, VAD thresholds,
             * feature matrix) is intentionally kept; only the read position jumps to "now". */
            st.detect_epoch = epoch;
            st.next_hop_end = wp;
            last_wake_us = 0; /* do not count the streaming gap as a hop period */
            resyncs++;
            /* The feature matrix still ends with the audio that fired WAKE_CONFIRMED.
             * Run the frontend for a full matrix length of fresh hops before the model is
             * fed again, otherwise it would re-detect the same keyword. (Not a reset.) */
            st.warmup_hops = KWS_FE_CONTEXT_HOPS;
        } else if (rb_pos_diff(wp, st.next_hop_end) >
                   (int32_t)(rb_history(ring) - SIH_WINDOW_SAMPLES)) {
            /* Fell further behind than the ring can cover: skip to the newest hop. */
            st.next_hop_end = wp;
            resyncs++;
        }

        uint32_t hops = 0;
        uint32_t wraps = 0;
        uint32_t errors = 0;
        uint32_t nn_notifies = 0;
        uint32_t nn_skipped = 0;
        uint32_t ran = 0;
        uint32_t gated = 0;
        while (rb_pos_diff(wp, st.next_hop_end) >= 0) {
            const uint32_t start = st.next_hop_end - SIH_WINDOW_SAMPLES;
            const rb_status_t r = rb_read(ring, start, st.window, SIH_WINDOW_SAMPLES);
            if (r != RB_OK) {
                errors++;
                st.next_hop_end = wp + SIH_HOP_SAMPLES; /* wait for the next hop, no spin */
                break;
            }
            if ((start & ring->mask) + SIH_WINDOW_SAMPLES > ring->capacity) {
                wraps++; /* this window was stitched across the physical end of the ring */
            }

            /* The frontend windows st.window in place; that is fine, it is our scratch copy. */
            const kws_fe_hop_t hop = kws_fe_process_hop(st.window);
            if (hop.ran_dsp) {
                ran++;
            } else {
                gated++;
            }

            st.next_hop_end += SIH_HOP_SAMPLES;
            hops++;

            if (st.warmup_hops > 0) {
                st.warmup_hops--; /* frontend ran (matrix refills), its invoke decision is ignored */
            } else if (hop.invoke_nn) {
                kws_fe_matrix_t *snap = pipeline_snapshot_acquire();
                if (snap == NULL) {
                    nn_skipped++; /* nn_task still busy with the previous snapshot */
                } else {
                    kws_fe_snapshot(snap);
                    pipeline_snapshot_publish(); /* notifies nn_task */
                    nn_notifies++;
                }
            }
            wp = rb_write_pos(ring); /* pick up hops that landed while we worked */
        }

        const int64_t t_done = esp_timer_get_time();
        const uint32_t proc_us = (uint32_t)(t_done - t_wake);
        const uint32_t period_us = (last_wake_us != 0) ? (uint32_t)(t_wake - last_wake_us) : 0;
        last_wake_us = t_wake;

        taskENTER_CRITICAL(&s_timing_lock);
        s_timing.wakes++;
        s_timing.hops += hops;
        if (period_us != 0) {
            s_timing.period_n++;
            s_timing.period_sum_us += period_us;
            if (period_us < s_timing.period_min_us) {
                s_timing.period_min_us = period_us;
            }
            if (period_us > s_timing.period_max_us) {
                s_timing.period_max_us = period_us;
            }
        }
        s_timing.proc_sum_us += proc_us;
        if (proc_us > s_timing.proc_max_us) {
            s_timing.proc_max_us = proc_us;
        }
        if (hops > s_timing.max_hops_per_wake) {
            s_timing.max_hops_per_wake = hops;
        }
        s_timing.wrap_windows += wraps;
        s_timing.read_errors += errors;
        s_timing.resyncs += resyncs;
        s_timing.nn_notifies += nn_notifies;
        s_timing.nn_skipped += nn_skipped;
        s_timing.dsp_ran += ran;
        s_timing.dsp_gated += gated;
        taskEXIT_CRITICAL(&s_timing_lock);
    }
}
