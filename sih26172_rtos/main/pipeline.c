/*
 * pipeline.c - see pipeline.h
 *
 * DESIGN CHOICE (open question #3, DSP/NN suspend mechanism): cooperative mode flag,
 * NOT vTaskSuspend(). See implementation.md "Interfaces" item 3 and "Open questions".
 *
 *   - A single atomic pipeline mode (DETECT / STREAM / COOLDOWN) replaces option (a)'s
 *     "shared flag nn_task checks". It is written by exactly two places:
 *       nn_task      DETECT -> STREAM      in pipeline_emit_wake_confirmed()
 *       network_task STREAM -> COOLDOWN -> DETECT   at teardown / end of cooldown
 *   - audio_ingest_task reads the mode on every hop and only notifies dsp_task in
 *     DETECT mode, so while streaming dsp_task is never woken and costs zero CPU. In
 *     STREAM mode the same per-hop notification goes to network_task instead.
 *   - dsp_task and nn_task also check the mode each time they wake (covers the one hop
 *     in flight at the moment of the switch) and "park": they block on their
 *     notification until network_task sends the resume notification after cooldown.
 *
 *   Why not vTaskSuspend(): suspending a task from another core stops it at an
 *   arbitrary instruction. If that happens while it holds a lock (the ESP_LOG /
 *   stdout lock, a newlib lock, a driver mutex) every other task that needs that lock
 *   deadlocks until resume. With the flag, the tasks always stop at a known safe point
 *   (the top of their loop) and dsp_state_t is never left half-updated.
 *
 *   The DSP frontend state (kws_fe: PCEN, VAD tiers, feature matrix) is not reset
 *   (implementation.md "Cooldown"). The ring keeps being written in every mode; on resume
 *   dsp_task jumps its read position to "now" and runs KWS_FE_CONTEXT_HOPS hops before it
 *   hands snapshots to nn_task again, so the model never sees the stale keyword frames.
 */
#include "pipeline.h"

#include <stdatomic.h>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

static const char *TAG = "pipeline";

#define START_BIT (1u << 0)

/* Ring buffer storage lives in internal SRAM (.bss). 32-bit aligned by type. */
static q15_16_t s_ring_storage[SIH_RING_CAPACITY];
static ring_buffer_t s_ring;

static StaticEventGroup_t s_start_group_buf;
static EventGroupHandle_t s_start_group;

/* 1-slot mailbox for WAKE_CONFIRMED (xQueueOverwrite: the newest event wins). */
static StaticQueue_t s_wake_queue_buf;
static uint8_t s_wake_queue_storage[sizeof(wake_confirmed_t)];
static QueueHandle_t s_wake_queue;

/* Shared feature-matrix snapshot (100 x 40 x int32 = 16000 B) and its ownership flag. */
static kws_fe_matrix_t s_snapshot;
static _Atomic bool s_snapshot_ready;

static pipeline_tasks_t s_tasks;
static _Atomic int s_mode = PIPE_MODE_DETECT;
static _Atomic uint32_t s_detect_epoch = 0;

void pipeline_init(void)
{
    const bool ok = rb_init(&s_ring, s_ring_storage, SIH_RING_CAPACITY, SIH_RING_MAX_WRITE, 0);
    configASSERT(ok);
    (void)ok;

    s_start_group = xEventGroupCreateStatic(&s_start_group_buf);
    configASSERT(s_start_group != NULL);

    s_wake_queue = xQueueCreateStatic(1, sizeof(wake_confirmed_t), s_wake_queue_storage,
                                      &s_wake_queue_buf);
    configASSERT(s_wake_queue != NULL);

    atomic_store(&s_mode, PIPE_MODE_DETECT);
}

void pipeline_start(const pipeline_tasks_t *tasks)
{
    s_tasks = *tasks;
    /* The event group call is a full barrier: every task that returns from
     * pipeline_wait_start() sees the handles written above. */
    xEventGroupSetBits(s_start_group, START_BIT);
}

void pipeline_wait_start(void)
{
    xEventGroupWaitBits(s_start_group, START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

const pipeline_tasks_t *pipeline_tasks(void)
{
    return &s_tasks;
}

ring_buffer_t *pipeline_ring(void)
{
    return &s_ring;
}

pipe_mode_t pipeline_mode(void)
{
    return (pipe_mode_t)atomic_load_explicit(&s_mode, memory_order_acquire);
}

void pipeline_set_mode(pipe_mode_t mode)
{
    atomic_store_explicit(&s_mode, (int)mode, memory_order_release);
}

const char *pipeline_mode_name(pipe_mode_t mode)
{
    switch (mode) {
    case PIPE_MODE_DETECT:   return "DETECT";
    case PIPE_MODE_STREAM:   return "STREAM";
    case PIPE_MODE_COOLDOWN: return "COOLDOWN";
    default:                 return "?";
    }
}

void pipeline_emit_wake_confirmed(const wake_confirmed_t *evt)
{
    /* T1-critical path: no logging, no blocking. Order matters:
     * 1. switch mode so the very next DMA hop is routed to network_task (DSP/NN bypass),
     * 2. publish the event,
     * 3. wake network_task. It runs as soon as nn_task blocks again (same core,
     *    lower priority), which nn_task does immediately after this call. */
    pipeline_set_mode(PIPE_MODE_STREAM);
    xQueueOverwrite(s_wake_queue, evt);
    xTaskNotify(s_tasks.net, NET_EVT_WAKE, eSetBits);
}

bool pipeline_take_wake_confirmed(wake_confirmed_t *out)
{
    return xQueueReceive(s_wake_queue, out, 0) == pdTRUE;
}

void pipeline_resume_detection(void)
{
    /* Epoch first, then the mode (release): a task that sees DETECT sees the new epoch. */
    atomic_fetch_add_explicit(&s_detect_epoch, 1u, memory_order_relaxed);
    pipeline_set_mode(PIPE_MODE_DETECT);
    /* dsp_task parks on its counting notification; one give wakes it. */
    xTaskNotifyGive(s_tasks.dsp);
    xTaskNotify(s_tasks.nn, NN_EVT_RESUME, eSetBits);
    ESP_LOGI(TAG, "mode -> DETECT (DSP/NN resumed, dsp_state_t preserved)");
}

kws_fe_matrix_t *pipeline_snapshot_acquire(void)
{
    if (atomic_load_explicit(&s_snapshot_ready, memory_order_acquire)) {
        return NULL;
    }
    return &s_snapshot;
}

void pipeline_snapshot_publish(void)
{
    atomic_store_explicit(&s_snapshot_ready, true, memory_order_release);
    xTaskNotify(s_tasks.nn, NN_EVT_INFER, eSetBits);
}

const kws_fe_matrix_t *pipeline_snapshot_get(void)
{
    if (!atomic_load_explicit(&s_snapshot_ready, memory_order_acquire)) {
        return NULL;
    }
    return &s_snapshot;
}

void pipeline_snapshot_release(void)
{
    atomic_store_explicit(&s_snapshot_ready, false, memory_order_release);
}

uint32_t pipeline_detect_epoch(void)
{
    return atomic_load_explicit(&s_detect_epoch, memory_order_acquire);
}

size_t pipeline_static_bytes(void)
{
    return sizeof(s_ring_storage) + sizeof(s_ring) + sizeof(s_snapshot) + sizeof(s_start_group_buf) +
           sizeof(s_wake_queue_buf) + sizeof(s_wake_queue_storage) + sizeof(s_tasks);
}
