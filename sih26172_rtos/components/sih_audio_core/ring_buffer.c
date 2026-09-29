/*
 * ring_buffer.c - see ring_buffer.h for the model and the concurrency argument.
 */
#include "ring_buffer.h"

#include <string.h>

static bool is_pow2(uint32_t x)
{
    return x != 0u && (x & (x - 1u)) == 0u;
}

bool rb_init(ring_buffer_t *rb, q15_16_t *storage, uint32_t capacity, uint32_t max_write,
             uint32_t initial_pos)
{
    if (rb == NULL || storage == NULL || !is_pow2(capacity) || max_write == 0u ||
        max_write >= capacity) {
        return false;
    }
    memset(storage, 0, (size_t)capacity * sizeof(q15_16_t));
    rb->storage = storage;
    rb->capacity = capacity;
    rb->mask = capacity - 1u;
    rb->max_write = max_write;
    atomic_store_explicit(&rb->write_pos, initial_pos, memory_order_release);
    return true;
}

void rb_write(ring_buffer_t *rb, const q15_16_t *src, uint32_t n)
{
    if (n == 0u) {
        return;
    }
    if (n > rb->max_write) {
        /* Contract violation: readers only keep max_write samples of guard.
         * Clamp to the newest max_write samples so the guard still holds. */
        src += n - rb->max_write;
        n = rb->max_write;
    }

    /* Single writer: nobody else modifies write_pos. */
    const uint32_t wp = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);

    /* Make sure the previous publish of write_pos is visible to other cores before
     * any of the slots below start being overwritten (seqlock "begin write" barrier). */
    atomic_thread_fence(memory_order_seq_cst);

    const uint32_t idx = wp & rb->mask;
    const uint32_t first = (n <= rb->capacity - idx) ? n : (rb->capacity - idx);
    memcpy(&rb->storage[idx], src, (size_t)first * sizeof(q15_16_t));
    if (first < n) {
        memcpy(&rb->storage[0], src + first, (size_t)(n - first) * sizeof(q15_16_t));
    }

    /* Publish: everything written above happens-before a reader's acquire of wp + n. */
    atomic_store_explicit(&rb->write_pos, wp + n, memory_order_release);
}

uint32_t rb_write_pos(const ring_buffer_t *rb)
{
    return atomic_load_explicit(&rb->write_pos, memory_order_acquire);
}

rb_status_t rb_read(const ring_buffer_t *rb, uint32_t start, q15_16_t *dst, uint32_t n)
{
    if (rb == NULL || dst == NULL || n == 0u || n > rb_history(rb)) {
        return RB_BAD_ARG;
    }

    const uint32_t wp = atomic_load_explicit(&rb->write_pos, memory_order_acquire);

    /* The whole range must already be published. Checked first, with a signed
     * difference, so that a range in the future is never mistaken for an old one. */
    if (rb_pos_diff(wp, start + n) < 0) {
        return RB_NOT_YET_WRITTEN;
    }
    /* The oldest requested sample must still be outside the writer's reach. */
    if ((uint32_t)(wp - start) > rb_history(rb)) {
        return RB_OVERWRITTEN;
    }

    const uint32_t idx = start & rb->mask;
    const uint32_t first = (n <= rb->capacity - idx) ? n : (rb->capacity - idx);
    memcpy(dst, &rb->storage[idx], (size_t)first * sizeof(q15_16_t));
    if (first < n) {
        /* Window spans the physical wrap point: stitch the second half from index 0. */
        memcpy(dst + first, &rb->storage[0], (size_t)(n - first) * sizeof(q15_16_t));
    }

    /* Seqlock "validate": order the copies above before re-reading write_pos. If the
     * writer published (or may be in the middle of) writes that reach our range,
     * the copy may be torn and is rejected. */
    atomic_thread_fence(memory_order_acquire);
    const uint32_t wp_after = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    if ((uint32_t)(wp_after - start) > rb_history(rb)) {
        return RB_OVERWRITTEN;
    }
    return RB_OK;
}
