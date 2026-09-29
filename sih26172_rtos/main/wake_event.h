/*
 * wake_event.h - INTERFACE #2 with the NN team: the WAKE_CONFIRMED event.
 *
 * Emitted exactly once per detection by nn_task through pipeline_emit_wake_confirmed()
 * (pipeline.h), consumed by network_task via a task notification + 1-slot mailbox.
 *
 * timestamp_us is the project's T1 latency checkpoint ("edge confirms keyword").
 * Take it with esp_timer_get_time() at the moment the keyword is confirmed and call
 * pipeline_emit_wake_confirmed() immediately afterwards: do not log, allocate or do
 * any other work in between, because that gap is part of the measured T1 latency.
 * network_task logs the handoff delay (timestamp -> network_task running) per session.
 */
#pragma once

#include <stdint.h>

typedef struct {
    float confidence;      /* model posterior / score for the detected keyword, 0..1 */
    int64_t timestamp_us;  /* esp_timer_get_time() when the keyword was confirmed */
} wake_confirmed_t;
