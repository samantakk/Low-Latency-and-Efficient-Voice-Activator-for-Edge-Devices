/*
 * pipeline.h - shared plumbing between the four tasks:
 *   - the audio ring buffer (static storage)
 *   - task handles and the start barrier
 *   - the pipeline mode (DETECT / STREAM / COOLDOWN) that implements the
 *     "DSP/NN suspend during streaming" decision (see DESIGN CHOICE in pipeline.c)
 *   - notification bit definitions
 *   - the WAKE_CONFIRMED handoff (interface #2)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kws_fe.h"
#include "ring_buffer.h"
#include "wake_event.h"

typedef enum {
    PIPE_MODE_DETECT = 0, /* ingest -> dsp -> nn running; network_task idle */
    PIPE_MODE_STREAM,     /* WAKE_CONFIRMED fired: ingest feeds network_task; dsp/nn parked */
    PIPE_MODE_COOLDOWN,   /* stream over, radio down: ring keeps filling, nobody else runs */
} pipe_mode_t;

/* ---- task notification bits: network_task (eSetBits) ---- */
#define NET_EVT_WAKE          (1u << 0)  /* WAKE_CONFIRMED waiting in the mailbox */
#define NET_EVT_AUDIO         (1u << 1)  /* a new hop was written to the ring (STREAM mode only) */
#define NET_EVT_WIFI_UP       (1u << 2)  /* got IP */
#define NET_EVT_WIFI_DOWN     (1u << 3)  /* station disconnected */
#define NET_EVT_WIFI_RETRY    (1u << 4)  /* Wi-Fi reconnect back-off timer expired */
#define NET_EVT_WS_CONNECTED  (1u << 5)  /* WebSocket upgrade complete */
#define NET_EVT_WS_DOWN       (1u << 6)  /* WebSocket disconnected / closed / task finished */
#define NET_EVT_SERVER_EOS    (1u << 7)  /* server sent {"type":"end_of_speech"} */

/* ---- task notification bits: nn_task (eSetBits) ---- */
#define NN_EVT_INFER          (1u << 0)  /* from dsp_task: new features, run the model */
#define NN_EVT_BUTTON         (1u << 1)  /* stub trigger: button ISR */
#define NN_EVT_RESUME         (1u << 2)  /* from network_task: cooldown finished */

typedef struct {
    TaskHandle_t ingest;
    TaskHandle_t dsp;
    TaskHandle_t nn;
    TaskHandle_t net;
    TaskHandle_t stats; /* may be NULL when diagnostics are disabled */
} pipeline_tasks_t;

/* Called once from app_main before any task is created. */
void pipeline_init(void);

/* Called once from app_main after all tasks are created; releases the start barrier. */
void pipeline_start(const pipeline_tasks_t *tasks);

/* Every task calls this first; returns after pipeline_start(). */
void pipeline_wait_start(void);

const pipeline_tasks_t *pipeline_tasks(void);
ring_buffer_t *pipeline_ring(void);

pipe_mode_t pipeline_mode(void);
void pipeline_set_mode(pipe_mode_t mode);
const char *pipeline_mode_name(pipe_mode_t mode);

/*
 * nn_task side of interface #2. Switches the pipeline to STREAM (so DSP/NN stop being
 * scheduled from the next hop on), posts the event and wakes network_task.
 * Safe to call from nn_task only. Does not block, does not log.
 */
void pipeline_emit_wake_confirmed(const wake_confirmed_t *evt);

/* network_task side: fetch the pending event (non-blocking). */
bool pipeline_take_wake_confirmed(wake_confirmed_t *out);

/* network_task, end of cooldown: back to DETECT and wake the parked dsp/nn tasks. */
void pipeline_resume_detection(void);

/* Incremented every time detection resumes. dsp_task compares it with the value it
 * last saw to know that a streaming/cooldown gap happened (even if it never woke
 * during the gap) and resyncs its read position without resetting dsp_state_t. */
uint32_t pipeline_detect_epoch(void);

/*
 * Feature-matrix snapshot: dsp_task (core 0) -> nn_task (core 1).
 *
 * The live feature matrix belongs to dsp_task and is appended to every 10 ms, while the model
 * needs a stable copy for the whole read. So dsp_task copies the matrix into this one shared
 * buffer and hands it over; nn_task quantizes it into the model input and releases it
 * before running the (slow) Invoke(). Ownership is a single flag, so no lock is needed:
 *   FREE  --dsp_task: acquire, fill, publish-->  READY  --nn_task: release-->  FREE
 * If nn_task still holds the previous snapshot, acquire fails and that invocation is
 * skipped (counted); dsp_task never waits.
 */
kws_fe_matrix_t *pipeline_snapshot_acquire(void);  /* dsp_task; NULL if nn_task still owns it */
void pipeline_snapshot_publish(void);              /* dsp_task; notifies nn_task (NN_EVT_INFER) */
const kws_fe_matrix_t *pipeline_snapshot_get(void);/* nn_task; NULL if nothing is READY */
void pipeline_snapshot_release(void);              /* nn_task; also used to drop a stale snapshot */

/* Bytes of static RAM owned by this module (ring storage, mailbox, barrier). */
size_t pipeline_static_bytes(void);
