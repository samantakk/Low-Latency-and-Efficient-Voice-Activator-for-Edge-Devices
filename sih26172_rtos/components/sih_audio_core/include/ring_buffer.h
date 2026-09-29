/*
 * ring_buffer.h - single-writer / multi-reader lock-free audio ring buffer
 *
 * Pure C11 (stdatomic), no FreeRTOS dependency, so it is unit tested on the host
 * (test/host/test_ring_buffer.c) before it ever runs on the ESP32-S3.
 *
 * Model
 *   - Every sample ever written has an absolute position (uint32_t, wraps after
 *     2^32 samples = ~74.5 hours at 16 kHz; all comparisons are wrap-safe).
 *   - The writer (audio_ingest_task only) publishes write_pos = one past the newest sample.
 *   - Readers (dsp_task, network_task) keep their own read positions and ask for
 *     any range [start, start + n). A read that crosses the physical end of the
 *     storage array is stitched together transparently (two memcpy calls).
 *   - The writer never blocks and never waits for readers. A reader that falls too
 *     far behind gets RB_OVERWRITTEN instead of silently receiving torn data.
 *
 * Concurrency (seqlock-style validation)
 *   The writer may be in the middle of writing up to `max_write` samples that are
 *   not yet published. A reader therefore only trusts samples that are at least
 *   `max_write` slots away from being overwritten, and it re-checks write_pos after
 *   copying. If the writer lapped the range while we copied, the read reports
 *   RB_OVERWRITTEN. Readable history = capacity - max_write samples.
 *
 * All storage is supplied by the caller (static allocation, no malloc).
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RB_OK = 0,           /* dst holds exactly the samples [start, start + n) */
    RB_NOT_YET_WRITTEN,  /* part of the range is in the future; nothing copied that can be used */
    RB_OVERWRITTEN,      /* part of the range was (or may have been) overwritten; dst is garbage */
    RB_BAD_ARG,
} rb_status_t;

typedef struct {
    q15_16_t *storage;
    uint32_t capacity;           /* power of two */
    uint32_t mask;               /* capacity - 1 */
    uint32_t max_write;          /* largest n ever passed to rb_write() */
    _Atomic uint32_t write_pos;  /* absolute position of the next sample to be written */
} ring_buffer_t;

/*
 * capacity must be a power of two, max_write >= 1 and max_write < capacity.
 * initial_pos is normally 0; tests use values near UINT32_MAX to exercise wrap of
 * the absolute position counter. storage is zeroed so that reads "before the
 * beginning of time" return silence.
 */
bool rb_init(ring_buffer_t *rb, q15_16_t *storage, uint32_t capacity, uint32_t max_write,
             uint32_t initial_pos);

/* Writer side. Single writer only. n must be <= max_write. Never blocks. */
void rb_write(ring_buffer_t *rb, const q15_16_t *src, uint32_t n);

/* Current write position (acquire). Samples [pos - rb_history(rb), pos) are readable. */
uint32_t rb_write_pos(const ring_buffer_t *rb);

/* Number of samples of history a reader can rely on. */
static inline uint32_t rb_history(const ring_buffer_t *rb)
{
    return rb->capacity - rb->max_write;
}

/* Oldest position that is still safely readable given a write position. */
static inline uint32_t rb_oldest_readable(const ring_buffer_t *rb, uint32_t write_pos)
{
    return write_pos - rb_history(rb);
}

/* Signed distance a - b between two absolute positions (wrap-safe). */
static inline int32_t rb_pos_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/*
 * Copy samples [start, start + n) into dst. 1 <= n <= rb_history(rb).
 * Any number of concurrent readers is fine; readers never modify the buffer.
 */
rb_status_t rb_read(const ring_buffer_t *rb, uint32_t start, q15_16_t *dst, uint32_t n);

#ifdef __cplusplus
}
#endif
