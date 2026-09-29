#pragma once

#include <stdint.h>

/*
 * nn_task: runs the NN team's model on feature-matrix snapshots from dsp_task and fires
 * WAKE_CONFIRMED (see nn_task.c). Also fires on the BOOT button for streaming bring-up.
 */
void nn_task(void *arg);

typedef struct {
    uint32_t invocations;     /* model inferences run in DETECT mode, this interval */
    uint32_t invoke_errors;   /* Invoke() failures, this interval */
    float conf_max;           /* highest model confidence seen, this interval */
    uint32_t triggers_total;  /* WAKE_CONFIRMED emitted since boot */
} nn_stats_t;

void nn_task_take_stats(nn_stats_t *out);
