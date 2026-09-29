#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * dsp_task: one 25 ms window per 10 ms hop from the ring buffer into the DSP team's
 * frontend (kws_fe.h); hands the feature matrix to nn_task when the frontend says so.
 */
void dsp_task(void *arg);

typedef struct {
    uint32_t wakes;              /* task wakeups in the interval */
    uint32_t hops;               /* hops (windows) processed in the interval */
    uint32_t period_min_us;      /* min/avg/max time between consecutive wakeups */
    uint32_t period_avg_us;
    uint32_t period_max_us;
    uint32_t proc_avg_us;        /* time spent per wakeup */
    uint32_t proc_max_us;
    uint32_t max_hops_per_wake;  /* 1 = never fell behind */
    uint32_t wrap_windows;       /* windows that spanned the ring's physical wrap point */
    uint32_t read_errors;        /* rb_read() did not return RB_OK */
    uint32_t resyncs;            /* read position jumped (resume after cooldown, or overrun) */
    uint32_t nn_notifies;        /* snapshots handed to nn_task */
    uint32_t nn_skipped;         /* invoke_nn true but nn_task still held the last snapshot */
    uint32_t dsp_ran;            /* hops where tier 1 passed and FFT/Mel/PCEN ran */
    uint32_t dsp_gated;          /* hops stopped by tier 1 (no DSP work) */
} dsp_timing_t;

/* Copy the interval statistics and reset them (called by stats_task). */
void dsp_task_take_timing(dsp_timing_t *out);

/* Static RAM of the DSP side (task bookkeeping + frontend + pipeline work buffers). */
size_t dsp_task_state_size(void);
