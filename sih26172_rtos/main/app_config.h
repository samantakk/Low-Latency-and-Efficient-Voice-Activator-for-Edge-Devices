/*
 * app_config.h - fixed architecture constants, task table, and compile-time checks.
 *
 * Tunables live in menuconfig (main/Kconfig.projbuild). Numbers here follow directly
 * from implementation.md and should only change together with that document.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "kws_fe.h"
#include "sdkconfig.h"

/* ---------------------------------------------------------------- audio timing */
#define SIH_SAMPLE_RATE_HZ      16000u
#define SIH_HOP_MS              10u
#define SIH_HOP_SAMPLES         (SIH_SAMPLE_RATE_HZ * SIH_HOP_MS / 1000u)    /* 160 */
#define SIH_WINDOW_MS           25u
#define SIH_WINDOW_SAMPLES      (SIH_SAMPLE_RATE_HZ * SIH_WINDOW_MS / 1000u) /* 400 */
#define SIH_SAMPLES_PER_MS      (SIH_SAMPLE_RATE_HZ / 1000u)                 /* 16 */

/* I2S: 32-bit words, mono. One DMA buffer == one hop (implementation.md "Ring buffer"). */
#define SIH_I2S_DMA_FRAMES      SIH_HOP_SAMPLES
#define SIH_I2S_WORD_BYTES      4u
#define SIH_I2S_DMA_BUF_BYTES   (SIH_I2S_DMA_FRAMES * SIH_I2S_WORD_BYTES)    /* 640 */

/* ---------------------------------------------------------------- ring buffer */
#define SIH_RING_CAPACITY       (1u << CONFIG_SIH_RING_CAPACITY_LOG2)
/* audio_ingest_task writes exactly one hop per rb_write() call. */
#define SIH_RING_MAX_WRITE      SIH_HOP_SAMPLES
#define SIH_RING_HISTORY        (SIH_RING_CAPACITY - SIH_RING_MAX_WRITE)
#define SIH_RING_HISTORY_MS     (SIH_RING_HISTORY / SIH_SAMPLES_PER_MS)

/* ---------------------------------------------------------------- streaming */
#define SIH_PREROLL_SAMPLES     ((uint32_t)CONFIG_SIH_PREROLL_MS * SIH_SAMPLES_PER_MS)
#define SIH_BACKLOG_SAMPLES     ((uint32_t)CONFIG_SIH_STREAM_BACKLOG_MS * SIH_SAMPLES_PER_MS)
#define SIH_WS_MIN_FRAME_SAMPLES ((uint32_t)CONFIG_SIH_WS_MIN_FRAME_MS * SIH_SAMPLES_PER_MS)
#define SIH_WS_MAX_FRAME_SAMPLES ((uint32_t)CONFIG_SIH_WS_MAX_FRAME_MS * SIH_SAMPLES_PER_MS)
/* esp_websocket_client internal TX/RX buffer: one max-size frame fits without fragmenting. */
#define SIH_WS_BUFFER_BYTES     (SIH_WS_MAX_FRAME_SAMPLES * 2u)

/* ---------------------------------------------------------------- diagnostics */
#if CONFIG_SIH_STATS_ENABLE
#define SIH_STATS_PERIOD_HOPS   ((uint32_t)CONFIG_SIH_STATS_PERIOD_HOPS)
#else
#define SIH_STATS_PERIOD_HOPS   0u
#endif

/* ---------------------------------------------------------------- DSP / NN contract
 * The DSP sector (kws_fe.h, checked against its C++ headers at build time) fixes the audio
 * framing, the feature matrix and the NN cadence. The RTOS constants above must agree. */
_Static_assert(KWS_FE_SAMPLE_RATE_HZ == SIH_SAMPLE_RATE_HZ, "DSP sample rate != I2S sample rate");
_Static_assert(KWS_FE_HOP_SAMPLES == SIH_HOP_SAMPLES, "DSP hop != RTOS hop (DMA buffer / notify cadence)");
_Static_assert(KWS_FE_FRAME_SAMPLES == SIH_WINDOW_SAMPLES, "DSP window != RTOS window (ring read size)");

/* ---------------------------------------------------------------- tasks
 *
 * | Task              | Core | Priority | Wakes on                               |
 * |-------------------|------|----------|----------------------------------------|
 * | audio_ingest_task | 0    | 21 (hi)  | I2S DMA-complete ISR (every 10 ms)     |
 * | dsp_task          | 0    | 19       | audio_ingest_task, every hop            |
 * | nn_task           | 1    | 10       | dsp_task (every 5 hops) / stub button   |
 * | network_task      | 1    | 7        | WAKE_CONFIRMED, then per-hop audio      |
 * | stats_task        | 1    | 2        | audio_ingest_task every N hops (diag.)  |
 *
 * Reference points in ESP-IDF v5.x (configMAX_PRIORITIES = 25):
 *   ipc tasks 24, esp_timer 22, Wi-Fi task 23 (moved to core 1 in sdkconfig.defaults),
 *   default event loop 20 (core 0), lwIP tcpip 18 (moved to core 1), websocket client 5.
 * audio_ingest_task sits above the event loop on core 0 so Wi-Fi event handling can
 * never delay a hop. dsp_task (19) sits below the default event loop (20, core 0); a
 * rare Wi-Fi event can delay it by a few ms, which it absorbs by processing every
 * pending hop when it wakes (counting notification).
 */
#define SIH_CORE_INGEST         0
#define SIH_CORE_DSP            0
#define SIH_CORE_NN             1
#define SIH_CORE_NET            1
#define SIH_CORE_STATS          1

#define SIH_PRIO_INGEST         21
#define SIH_PRIO_DSP            19
#define SIH_PRIO_NN             10
#define SIH_PRIO_NET            7
#define SIH_PRIO_STATS          2
#define SIH_PRIO_WS_CLIENT      5   /* esp_websocket_client's own task, pinned to core 1 */

/* Stack sizes in BYTES (ESP-IDF FreeRTOS: StackType_t is uint8_t).
 * The stats task prints each task's high-water mark so these can be trimmed. */
#define SIH_STACK_INGEST        3072
#define SIH_STACK_DSP           4096   /* C++ frontend call depth + ESP_LOG; trim from stats HWM */
#define SIH_STACK_NN            8192   /* TFLite Micro Invoke(); the NN team runs it on an 8 KB stack */
#define SIH_STACK_NET           6144
#define SIH_STACK_STATS         4096
#define SIH_STACK_WS_CLIENT     8192   /* runs the mbedTLS handshake for wss:// */

/* ---------------------------------------------------------------- compile-time checks */
_Static_assert(SIH_HOP_SAMPLES == 160, "10 ms @ 16 kHz must be 160 samples");
_Static_assert(SIH_I2S_DMA_BUF_BYTES <= 4092, "DMA buffer larger than one GDMA descriptor");
_Static_assert(SIH_WINDOW_SAMPLES + SIH_HOP_SAMPLES <= SIH_RING_HISTORY,
               "Ring buffer must hold at least one 25 ms window plus margin");
_Static_assert(SIH_PREROLL_SAMPLES + 4u * SIH_HOP_SAMPLES <= SIH_RING_HISTORY,
               "Pre-roll does not fit in the ring buffer history: raise "
               "SIH_RING_CAPACITY_LOG2 or lower SIH_PREROLL_MS");
/* esp_websocket_client applies the send timeout up to 4 times per frame (lock, poll,
 * header write, payload write), so the worst-case block is 4x the configured value. */
_Static_assert(4u * (uint32_t)CONFIG_SIH_WS_SEND_TIMEOUT_MS + 4u * SIH_HOP_MS < SIH_RING_HISTORY_MS,
               "A blocked WebSocket send (up to 4x SIH_WS_SEND_TIMEOUT_MS) could outlast the ring "
               "buffer history: raise SIH_RING_CAPACITY_LOG2 or lower SIH_WS_SEND_TIMEOUT_MS");
_Static_assert(SIH_BACKLOG_SAMPLES > SIH_PREROLL_SAMPLES + SIH_WS_MAX_FRAME_SAMPLES,
               "Streaming backlog must be larger than the pre-roll plus one frame");
_Static_assert(CONFIG_SIH_WS_MIN_FRAME_MS <= CONFIG_SIH_WS_MAX_FRAME_MS,
               "SIH_WS_MIN_FRAME_MS must not exceed SIH_WS_MAX_FRAME_MS");
_Static_assert(SIH_WS_MAX_FRAME_SAMPLES <= SIH_BACKLOG_SAMPLES, "frame larger than backlog");
