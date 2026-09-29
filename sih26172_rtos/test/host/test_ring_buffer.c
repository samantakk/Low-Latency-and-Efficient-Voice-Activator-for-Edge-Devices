/*
 * Host unit tests for components/sih_audio_core (implementation.md build step 1:
 * "Ring buffer, standalone, unit-tested for wraparound correctness under continuous
 * writes"). Also checks the I2S word -> Q15.16 -> PCM16 conversions.
 *
 * Build and run:  ./run_tests.sh   (needs gcc or clang with pthreads)
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_format.h"
#include "ring_buffer.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

#define HOP 160u
#define WINDOW 400u

/* The value stored for absolute position p: unique, easy to verify. */
static q15_16_t value_at(uint32_t p)
{
    return (q15_16_t)(p * 2654435761u); /* Knuth hash, wraps mod 2^32 like positions do */
}

static void write_hops(ring_buffer_t *rb, uint32_t hops)
{
    q15_16_t hop[HOP];
    for (uint32_t h = 0; h < hops; h++) {
        const uint32_t wp = rb_write_pos(rb);
        for (uint32_t i = 0; i < HOP; i++) {
            hop[i] = value_at(wp + i);
        }
        rb_write(rb, hop, HOP);
    }
}

static int window_matches(const q15_16_t *w, uint32_t start, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != value_at(start + i)) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ conversions */
static void test_conversions(void)
{
    /* 18-bit sample MSB-aligned in a 32-bit word (SPH0645 layout). */
    const unsigned B = 18;
    CHECK(i2s_word_to_q15_16(0x00000000u, B) == 0, "zero");
    /* max positive 18-bit = 0x1FFFF -> 131071 >> 1 = 65535 (just below 1.0) */
    CHECK(i2s_word_to_q15_16(0x1FFFFu << 14, B) == 65535, "max positive: %d", i2s_word_to_q15_16(0x1FFFFu << 14, B));
    /* min negative 18-bit = -131072 -> -65536 == -1.0 exactly */
    CHECK(i2s_word_to_q15_16(0x20000u << 14, B) == -65536, "min negative: %d", i2s_word_to_q15_16(0x20000u << 14, B));
    /* -1 LSB (all 18 bits set) -> floor(-0.5) = -1 */
    CHECK(i2s_word_to_q15_16(0x3FFFFu << 14, B) == -1, "minus one LSB");
    /* padding bits below the sample are ignored whatever they contain */
    CHECK(i2s_word_to_q15_16(0x00003FFFu, B) == 0, "padding ignored (zero sample)");
    CHECK(i2s_word_to_q15_16((0x10000u << 14) | 0x3FFFu, B) == 32768, "padding ignored (+0.5)");
    /* +0.5 full scale: 18-bit value 65536 -> 32768 in Q15.16 */
    CHECK(i2s_word_to_q15_16(0x10000u << 14, B) == 32768, "+0.5");
    CHECK(i2s_word_to_q15_16(0x30000u << 14, B) == -32768, "-0.5");

    /* 24-bit (INMP441): 0x400000 = +0.5 */
    CHECK(i2s_word_to_q15_16(0x400000u << 8, 24) == 32768, "24-bit +0.5");
    CHECK(i2s_word_to_q15_16(0x800000u << 8, 24) == -65536, "24-bit -1.0");
    /* 16-bit: 0x4000 = +0.5 */
    CHECK(i2s_word_to_q15_16(0x4000u << 16, 16) == 32768, "16-bit +0.5");
    CHECK(i2s_word_to_q15_16(0x8000u << 16, 16) == -65536, "16-bit -1.0");
    CHECK(i2s_word_to_q15_16(0x7FFFu << 16, 16) == 65534, "16-bit max");
    /* 32-bit valid bits */
    CHECK(i2s_word_to_q15_16(0x40000000u, 32) == 32768, "32-bit +0.5");

    /* Q15.16 -> PCM16 */
    CHECK(q15_16_to_pcm16(0) == 0, "pcm zero");
    CHECK(q15_16_to_pcm16(32768) == 16384, "pcm +0.5");
    CHECK(q15_16_to_pcm16(-65536) == -32768, "pcm -1.0");
    CHECK(q15_16_to_pcm16(65535) == 32767, "pcm just below 1.0");
    CHECK(q15_16_to_pcm16(65536) == 32767, "pcm +1.0 saturates");
    CHECK(q15_16_to_pcm16(1 << 20) == 32767, "pcm overload saturates");
    CHECK(q15_16_to_pcm16(-(1 << 20)) == -32768, "pcm negative overload saturates");
}

/* ------------------------------------------------------------------ single-threaded */
static void test_init_args(void)
{
    static q15_16_t st[1024];
    ring_buffer_t rb;
    CHECK(!rb_init(&rb, st, 1000, HOP, 0), "non power of two rejected");
    CHECK(!rb_init(&rb, st, 1024, 0, 0), "max_write 0 rejected");
    CHECK(!rb_init(&rb, st, 1024, 1024, 0), "max_write == capacity rejected");
    CHECK(!rb_init(&rb, NULL, 1024, HOP, 0), "NULL storage rejected");
    CHECK(rb_init(&rb, st, 1024, HOP, 0), "valid init");
    q15_16_t d[8];
    CHECK(rb_read(&rb, 0, d, 0) == RB_BAD_ARG, "n = 0");
    CHECK(rb_read(&rb, 0, d, 1024 - HOP + 1) == RB_BAD_ARG, "n > history");
}

static void test_windows_across_wrap(uint32_t capacity, uint32_t initial_pos)
{
    q15_16_t *st = calloc(capacity, sizeof(q15_16_t));
    ring_buffer_t rb;
    CHECK(rb_init(&rb, st, capacity, HOP, initial_pos), "init cap=%" PRIu32, capacity);

    q15_16_t win[WINDOW];
    uint32_t next_hop_end = initial_pos + HOP;
    uint32_t windows = 0, wrapped = 0, bad = 0;

    /* Enough hops to wrap the physical buffer many times (and the absolute counter,
     * when initial_pos is close to UINT32_MAX). Mimics dsp_task exactly. */
    const uint32_t hops = (capacity / HOP) * 40u + 7u;
    for (uint32_t h = 0; h < hops; h++) {
        write_hops(&rb, 1);
        const uint32_t wp = rb_write_pos(&rb);
        while (rb_pos_diff(wp, next_hop_end) >= 0) {
            const uint32_t start = next_hop_end - WINDOW;
            const bool before_time = rb_pos_diff(start, initial_pos) < 0;
            const rb_status_t r = rb_read(&rb, start, win, WINDOW);
            if (before_time) {
                /* The very first window reaches back before the first write: allowed,
                 * the missing part reads as zeros (silence). */
                CHECK(r == RB_OK, "first window readable");
            } else {
                if (r != RB_OK || !window_matches(win, start, WINDOW)) {
                    bad++;
                }
                if ((start & rb.mask) + WINDOW > rb.capacity) {
                    wrapped++;
                }
                windows++;
            }
            next_hop_end += HOP;
        }
    }
    CHECK(bad == 0, "cap=%" PRIu32 " init=%" PRIu32 ": %" PRIu32 " bad windows of %" PRIu32, capacity, initial_pos,
          bad, windows);
    CHECK(wrapped > 0, "cap=%" PRIu32 ": no window crossed the physical wrap point", capacity);
    printf("  capacity %5" PRIu32 " start 0x%08" PRIx32 ": %6" PRIu32 " windows OK, %4" PRIu32
           " spanned the wrap point\n",
           capacity, initial_pos, windows, wrapped);
    free(st);
}

static void test_bounds(void)
{
    enum { CAP = 2048 };
    static q15_16_t st[CAP];
    ring_buffer_t rb;
    rb_init(&rb, st, CAP, HOP, 0);
    write_hops(&rb, 100); /* 16000 samples written, capacity 2048 */
    const uint32_t wp = rb_write_pos(&rb);
    q15_16_t d[CAP];
    const uint32_t hist = rb_history(&rb);

    CHECK(rb_read(&rb, wp - 10, d, 11) == RB_NOT_YET_WRITTEN, "range ending in the future");
    CHECK(rb_read(&rb, wp + 5, d, 1) == RB_NOT_YET_WRITTEN, "range fully in the future");
    CHECK(rb_read(&rb, wp - 1, d, 1) == RB_OK && d[0] == value_at(wp - 1), "newest sample");
    CHECK(rb_read(&rb, wp - hist, d, hist) == RB_OK && window_matches(d, wp - hist, hist), "entire history");
    CHECK(rb_read(&rb, wp - hist - 1, d, 1) == RB_OVERWRITTEN, "one sample older than history");
    CHECK(rb_read(&rb, wp - CAP, d, 16) == RB_OVERWRITTEN, "older than capacity");
    CHECK(rb_read(&rb, 0, d, 16) == RB_OVERWRITTEN, "ancient data");

    /* network_task style: drain arbitrary chunk sizes, crossing wrap, exact continuity */
    uint32_t pos = wp - hist;
    uint32_t chunk = 1;
    while (rb_pos_diff(rb_write_pos(&rb), pos) > 0) {
        uint32_t avail = (uint32_t)rb_pos_diff(rb_write_pos(&rb), pos);
        uint32_t n = chunk < avail ? chunk : avail;
        CHECK(rb_read(&rb, pos, d, n) == RB_OK && window_matches(d, pos, n), "drain chunk %" PRIu32, n);
        pos += n;
        chunk = (chunk * 7u + 3u) % 700u + 1u;
    }
    CHECK(pos == rb_write_pos(&rb), "drained exactly to write_pos");

    /* oversize write is clamped to max_write and keeps the newest samples */
    q15_16_t big[HOP * 2];
    const uint32_t w0 = rb_write_pos(&rb);
    for (uint32_t i = 0; i < HOP * 2; i++) {
        big[i] = (q15_16_t)(0x5000 + i);
    }
    rb_write(&rb, big, HOP * 2);
    CHECK(rb_write_pos(&rb) == w0 + HOP, "oversize write clamped");
    CHECK(rb_read(&rb, w0, d, HOP) == RB_OK && d[0] == 0x5000 + (q15_16_t)HOP && d[HOP - 1] == 0x5000 + 2 * (q15_16_t)HOP - 1,
          "oversize write kept newest samples");
}

/* ------------------------------------------------------------------ concurrent stress */
typedef struct {
    ring_buffer_t *rb;
    atomic_bool stop;
    uint32_t hops_to_write;
} writer_ctx_t;

static void *writer_thread(void *arg)
{
    writer_ctx_t *c = arg;
    for (uint32_t h = 0; h < c->hops_to_write; h++) {
        write_hops(c->rb, 1);
        if ((h & 63u) == 0) {
            sched_yield();
        }
    }
    atomic_store(&c->stop, true);
    return NULL;
}

typedef struct {
    ring_buffer_t *rb;
    writer_ctx_t *w;
    uint32_t initial_pos;
    uint32_t ok, overwritten, not_yet, torn;
    uint32_t window;
    int lag_mode; /* 0 = keep up, 1 = deliberately fall behind sometimes */
} reader_ctx_t;

static void *reader_thread(void *arg)
{
    reader_ctx_t *c = arg;
    q15_16_t *buf = malloc(sizeof(q15_16_t) * c->window);
    uint32_t rnd = 12345u;
    while (!atomic_load(&c->w->stop)) {
        const uint32_t wp = rb_write_pos(c->rb);
        /* Until the ring has been filled once, reads before initial_pos legitimately
         * return the zero-initialized "silence before the beginning of time". */
        if ((uint32_t)(wp - c->initial_pos) < 2u * c->rb->capacity + 2000u) {
            sched_yield();
            continue;
        }
        rnd = rnd * 1103515245u + 12345u;
        uint32_t back = c->window + (c->lag_mode ? (rnd >> 8) % (c->rb->capacity + 400u) : (rnd >> 8) % 600u);
        const uint32_t start = wp - back;
        const rb_status_t r = rb_read(c->rb, start, buf, c->window);
        if (r == RB_OK) {
            if (!window_matches(buf, start, c->window)) {
                c->torn++; /* the one thing that must never happen */
            }
            c->ok++;
        } else if (r == RB_OVERWRITTEN) {
            c->overwritten++;
        } else if (r == RB_NOT_YET_WRITTEN) {
            c->not_yet++;
        }
    }
    free(buf);
    return NULL;
}

static void test_concurrent(uint32_t capacity)
{
    q15_16_t *st = calloc(capacity, sizeof(q15_16_t));
    ring_buffer_t rb;
    const uint32_t initial_pos = 0xFFF00000u; /* also crosses the 2^32 position wrap */
    rb_init(&rb, st, capacity, HOP, initial_pos);
    writer_ctx_t w = {.rb = &rb, .stop = false, .hops_to_write = 400000u};
    reader_ctx_t r1 = {.rb = &rb, .w = &w, .initial_pos = initial_pos, .window = WINDOW, .lag_mode = 0};
    reader_ctx_t r2 = {.rb = &rb, .w = &w, .initial_pos = initial_pos, .window = HOP, .lag_mode = 1};
    pthread_t tw, t1, t2;
    pthread_create(&tw, NULL, writer_thread, &w);
    pthread_create(&t1, NULL, reader_thread, &r1);
    pthread_create(&t2, NULL, reader_thread, &r2);
    pthread_join(tw, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK(r1.torn == 0 && r2.torn == 0, "torn reads reported as RB_OK: %" PRIu32 " / %" PRIu32, r1.torn, r2.torn);
    CHECK(r1.ok > 1000 && r2.ok > 1000, "too few successful concurrent reads (%" PRIu32 ", %" PRIu32 ")", r1.ok,
          r2.ok);
    printf("  concurrent cap %5" PRIu32 ": 64M samples written; dsp-like reader %" PRIu32 " OK, net-like reader %" PRIu32
           " OK / %" PRIu32 " overwritten (detected); torn = %" PRIu32 "\n",
           capacity, r1.ok, r2.ok, r2.overwritten, r1.torn + r2.torn);
    free(st);
}

int main(void)
{
    printf("audio format conversions\n");
    test_conversions();
    printf("ring buffer: argument checks\n");
    test_init_args();
    printf("ring buffer: 25 ms windows every 10 ms hop under continuous writes\n");
    test_windows_across_wrap(2048, 0);
    test_windows_across_wrap(8192, 0);
    test_windows_across_wrap(32768, 0);
    test_windows_across_wrap(8192, 0xFFFFF000u); /* absolute position counter wraps too */
    printf("ring buffer: bounds, overrun detection, chunked draining\n");
    test_bounds();
    printf("ring buffer: concurrent writer + 2 readers\n");
    test_concurrent(2048);
    test_concurrent(8192);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "TESTS FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
