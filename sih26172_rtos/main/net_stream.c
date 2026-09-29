/*
 * net_stream.c - network_task: the streaming pipeline (implementation.md
 * "Streaming pipeline (on WAKE_CONFIRMED)" steps 1-5).
 *
 *  idle:  blocked in xTaskNotifyWait(portMAX_DELAY). Zero CPU, zero wakeups except
 *         Wi-Fi link events.
 *  1. WAKE_CONFIRMED: take the event from the mailbox, freeze the pre-roll (copy it out
 *     of the ring buffer into the 16-bit backlog FIFO immediately).
 *  2. Wi-Fi up (wifi_link_session_begin; see DESIGN CHOICE in wifi_link.c).
 *  3. WebSocket: esp_websocket_client_start() (ws:// or wss:// with mbedTLS; the
 *     ESP32-S3 AES/SHA/RSA accelerators are enabled in sdkconfig.defaults).
 *  4. Send a small JSON "start" text frame, then pre-roll + live audio as binary frames
 *     of raw PCM s16le, 16 kHz, mono. From the trigger on, ingest notifies this task on
 *     every hop (NET_EVT_AUDIO), so the ring is drained into the backlog continuously,
 *     also while Wi-Fi / TLS are still connecting: nothing the user says is lost.
 *  5. End of speech (DESIGN CHOICE below) -> "stop" text frame, clean WebSocket close,
 *     Wi-Fi back to its idle policy, cooldown, resume DSP/NN.
 *
 * DESIGN CHOICE (open question #2, end of speech): hybrid, whichever comes first:
 *   a) local energy cutoff: after speech has been heard post-trigger, 800 ms of
 *      trailing silence ends the utterance (CONFIG_SIH_EOS_TRAILING_SILENCE_MS).
 *      "Speech" = hop energy (DC removed) 12 dB above a tracked noise floor AND above
 *      -55 dBFS. The noise floor is seeded from the quietest hop in the ring audio
 *      before the pre-roll, follows drops instantly and rises at 2 dB/s. This is deliberately simple and independent of the DSP
 *      team's Tier-0/1 VAD, which is suspended while streaming.
 *   b) no speech at all within 3 s after the trigger -> treat as a false trigger.
 *   c) hard cap: 8 s of post-trigger audio (CONFIG_SIH_EOS_MAX_STREAM_MS).
 *   d) server signal: a text frame containing "end_of_speech" (the ASR's own endpointer)
 *      ends it immediately (CONFIG_SIH_EOS_HONOR_SERVER_SIGNAL).
 *   All audio up to the end point is still sent before closing.
 *
 * DESIGN CHOICE (open question #4, cooldown): 1500 ms (CONFIG_SIH_COOLDOWN_MS), counted
 * from the moment the WebSocket is closed and the radio is back in its idle state.
 * dsp_state_t is not reset. Tune against real trailing-audio re-trigger tests.
 */
#include "net_stream.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "audio_format.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipeline.h"
#include "ring_buffer.h"
#include "wifi_link.h"

#if CONFIG_SIH_WS_TLS_CRT_BUNDLE
#include "esp_crt_bundle.h"
#else
/* Embedded by main/CMakeLists.txt from main/certs/server_cert.pem */
extern const char server_cert_pem_start[] asm("_binary_server_cert_pem_start");
#endif

static const char *TAG = "net";

/* ------------------------------------------------------------------ tunables (derived) */
#define WS_CLOSE_TIMEOUT_MS      2000
#define SESSION_WAIT_TICKS       pdMS_TO_TICKS(50) /* safety net; audio wakes us every 10 ms */
#define NOISE_FLOOR_RISE_DB_S    2.0f
#define EOS_MAX_SAMPLES          ((uint32_t)CONFIG_SIH_EOS_MAX_STREAM_MS * SIH_SAMPLES_PER_MS)
#define EOS_SILENCE_SAMPLES      ((uint32_t)CONFIG_SIH_EOS_TRAILING_SILENCE_MS * SIH_SAMPLES_PER_MS)
#define EOS_NO_SPEECH_SAMPLES    ((uint32_t)CONFIG_SIH_EOS_NO_SPEECH_TIMEOUT_MS * SIH_SAMPLES_PER_MS)
#define SESSION_HARD_LIMIT_US    ((int64_t)(CONFIG_SIH_WIFI_CONNECT_TIMEOUT_MS + CONFIG_SIH_WS_CONNECT_TIMEOUT_MS + \
                                            CONFIG_SIH_EOS_MAX_STREAM_MS + 5000) * 1000)
/* Keep a couple of hops of distance from the writer when jumping to the oldest audio. */
#define RING_SAFE_BACK_SAMPLES   (((SIH_RING_HISTORY / SIH_HOP_SAMPLES) - 2u) * SIH_HOP_SAMPLES)

#define WS_EVENT_BITS (NET_EVT_WS_CONNECTED | NET_EVT_WS_DOWN | NET_EVT_SERVER_EOS)

typedef enum {
    SESS_WAIT_WIFI = 0,
    SESS_WAIT_WS,
    SESS_STREAMING,
    SESS_DONE,
} sess_state_t;

typedef struct {
    uint32_t number;
    sess_state_t state;
    wake_confirmed_t wake;
    int64_t t_rx_us;            /* network_task running with the event in hand */
    int64_t t_wifi_us;          /* IP available (0 = never) */
    int64_t t_ws_us;            /* WebSocket upgraded (0 = never) */
    int64_t t_first_audio_us;   /* first audio frame handed to the TCP stack (0 = never) */
    int64_t deadline_us;        /* for the current WAIT_* state */

    uint32_t trigger_pos;       /* ring position that corresponds to wake.timestamp_us */
    uint32_t read_pos;          /* next ring position to drain into the backlog */
    uint32_t preroll_samples;   /* actually frozen before the trigger point */

    bool eos_marked;            /* end point decided: drain up to eos_pos, then close */
    uint32_t eos_pos;
    const char *end_reason;
    bool failed;

    /* end-of-speech detector */
    bool floor_init;
    float noise_floor;
    bool speech_seen;
    uint32_t silence_samples;
    uint32_t post_trigger_samples;

    /* accounting */
    uint32_t samples_sent;
    uint32_t frames_sent;
    uint32_t backlog_peak;
    uint32_t backlog_dropped;
    uint32_t ring_skipped;
} session_t;

/* ------------------------------------------------------------------ static storage */
static int16_t s_backlog[SIH_BACKLOG_SAMPLES];  /* PCM16 FIFO: pre-roll + live backlog */
static uint32_t s_bl_head;                      /* index of the oldest sample */
static uint32_t s_bl_count;
static q15_16_t s_chunk[SIH_HOP_SAMPLES];
static char s_text[320];
static session_t s_sess;

static esp_websocket_client_handle_t s_ws;
static TaskHandle_t s_self;
static _Atomic bool s_ws_task_alive;

static float s_margin_lin;       /* speech margin over the floor, as an energy ratio */
static float s_min_speech_lin;   /* absolute speech threshold, energy re full scale */
static float s_floor_rise;       /* per-hop multiplicative noise-floor rise */

uint32_t net_stream_static_bytes(void)
{
    return (uint32_t)(sizeof(s_backlog) + sizeof(s_chunk) + sizeof(s_text) + sizeof(s_sess));
}

/* ------------------------------------------------------------------ helpers */
static int64_t now_us(void)
{
    return esp_timer_get_time();
}

#if CONFIG_SIH_EOS_HONOR_SERVER_SIGNAL
/* Bounded substring search (WebSocket payloads are not NUL-terminated). */
static bool contains(const char *hay, int hay_len, const char *needle)
{
    const int n = (int)strlen(needle);
    for (int i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, (size_t)n) == 0) {
            return true;
        }
    }
    return false;
}
#endif

static void backlog_reset(void)
{
    s_bl_head = 0;
    s_bl_count = 0;
}

static void backlog_push(session_t *s, const q15_16_t *x, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (s_bl_count == SIH_BACKLOG_SAMPLES) {
            /* Full: drop the OLDEST sample so the most recent speech is kept. */
            s_bl_head = (s_bl_head + 1u == SIH_BACKLOG_SAMPLES) ? 0u : s_bl_head + 1u;
            s_bl_count--;
            s->backlog_dropped++;
        }
        uint32_t tail = s_bl_head + s_bl_count;
        if (tail >= SIH_BACKLOG_SAMPLES) {
            tail -= SIH_BACKLOG_SAMPLES;
        }
        s_backlog[tail] = q15_16_to_pcm16(x[i]);
        s_bl_count++;
    }
    if (s_bl_count > s->backlog_peak) {
        s->backlog_peak = s_bl_count;
    }
}

static void mark_eos(session_t *s, uint32_t pos, const char *reason)
{
    if (!s->eos_marked) {
        s->eos_marked = true;
        s->eos_pos = pos;
        s->end_reason = reason;
    }
}

/* Mean-square energy of a chunk with DC removed (MEMS mics have a DC offset). */
static float chunk_energy(const q15_16_t *x, uint32_t n)
{
    float sum = 0.0f;
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float f = sih_q15_16_to_float(x[i]);
        sum += f;
        sum_sq += f * f;
    }
    const float mean = sum / (float)n;
    const float energy = sum_sq / (float)n - mean * mean;
    return (energy < 1e-12f) ? 1e-12f : energy;
}

/* Seed the noise floor with the quietest hop in the ring audio OLDER than the pre-roll
 * (not sent), so a pre-roll that is all wake word does not inflate the floor. */
static void seed_noise_floor(session_t *s, uint32_t from, uint32_t to)
{
    const ring_buffer_t *ring = pipeline_ring();
    for (uint32_t p = from; rb_pos_diff(to, p) >= (int32_t)SIH_HOP_SAMPLES; p += SIH_HOP_SAMPLES) {
        if (rb_read(ring, p, s_chunk, SIH_HOP_SAMPLES) != RB_OK) {
            continue;
        }
        const float e = chunk_energy(s_chunk, SIH_HOP_SAMPLES);
        if (!s->floor_init || e < s->noise_floor) {
            s->noise_floor = e;
            s->floor_init = true;
        }
    }
}

/* End-of-speech detector, run on every chunk drained from the ring. */
static void eos_update(session_t *s, const q15_16_t *x, uint32_t n, uint32_t chunk_start)
{
    const float energy = chunk_energy(x, n);

    if (!s->floor_init) {
        s->noise_floor = energy;
        s->floor_init = true;
    } else if (energy < s->noise_floor) {
        s->noise_floor = energy;
    } else {
        s->noise_floor *= s_floor_rise;
    }

    /* Only audio after the trigger point counts toward end of speech; the pre-roll
     * (which contains the wake word) only seeds the noise floor. */
    const uint32_t chunk_end = chunk_start + n;
    if (rb_pos_diff(chunk_end, s->trigger_pos) <= 0 || s->eos_marked) {
        return;
    }

    const bool speech = energy > s->noise_floor * s_margin_lin && energy > s_min_speech_lin;
    s->post_trigger_samples += n;
    if (speech) {
        s->speech_seen = true;
        s->silence_samples = 0;
    } else {
        s->silence_samples += n;
    }

    if (s->post_trigger_samples >= EOS_MAX_SAMPLES) {
        mark_eos(s, chunk_end, "max_duration");
    } else if (s->speech_seen && s->silence_samples >= EOS_SILENCE_SAMPLES) {
        mark_eos(s, chunk_end, "trailing_silence");
    } else if (!s->speech_seen && s->post_trigger_samples >= EOS_NO_SPEECH_SAMPLES) {
        mark_eos(s, chunk_end, "no_speech");
    }
}

/* Copy everything new in the ring (up to the end point) into the backlog. */
static void drain_ring(session_t *s)
{
    const ring_buffer_t *ring = pipeline_ring();
    uint32_t wp = rb_write_pos(ring);

    if (rb_pos_diff(wp, s->read_pos) > (int32_t)SIH_RING_HISTORY) {
        /* We were blocked longer than the ring covers: skip what is gone. */
        const uint32_t new_pos = wp - RING_SAFE_BACK_SAMPLES;
        s->ring_skipped += (uint32_t)rb_pos_diff(new_pos, s->read_pos);
        s->read_pos = new_pos;
    }

    for (;;) {
        const uint32_t limit = s->eos_marked ? s->eos_pos : wp;
        const int32_t avail = rb_pos_diff(limit, s->read_pos);
        if (avail <= 0) {
            break;
        }
        const uint32_t n = ((uint32_t)avail < SIH_HOP_SAMPLES) ? (uint32_t)avail : SIH_HOP_SAMPLES;
        const rb_status_t r = rb_read(ring, s->read_pos, s_chunk, n);
        if (r == RB_OVERWRITTEN) {
            wp = rb_write_pos(ring);
            const uint32_t new_pos = wp - RING_SAFE_BACK_SAMPLES;
            s->ring_skipped += (uint32_t)rb_pos_diff(new_pos, s->read_pos);
            s->read_pos = new_pos;
            continue;
        }
        if (r != RB_OK) {
            break;
        }
        eos_update(s, s_chunk, n, s->read_pos);
        backlog_push(s, s_chunk, n);
        s->read_pos += n;
    }
}

static bool all_audio_queued(const session_t *s)
{
    return s->eos_marked && rb_pos_diff(s->eos_pos, s->read_pos) <= 0;
}

/* Send backlog audio. flush = send even less than the minimum frame size. */
static bool send_audio(session_t *s)
{
    while (s_bl_count > 0) {
        const bool flush = all_audio_queued(s) || s->frames_sent == 0;
        if (!flush && s_bl_count < SIH_WS_MIN_FRAME_SAMPLES) {
            break; /* batch live audio into >= MIN_FRAME frames */
        }
        uint32_t n = SIH_BACKLOG_SAMPLES - s_bl_head; /* contiguous part */
        if (n > s_bl_count) {
            n = s_bl_count;
        }
        if (n > SIH_WS_MAX_FRAME_SAMPLES) {
            n = SIH_WS_MAX_FRAME_SAMPLES;
        }
        const int bytes = (int)(n * sizeof(int16_t));
        const int sent = esp_websocket_client_send_bin(s_ws, (const char *)&s_backlog[s_bl_head], bytes,
                                                       pdMS_TO_TICKS(CONFIG_SIH_WS_SEND_TIMEOUT_MS));
        if (sent != bytes) {
            return false;
        }
        if (s->t_first_audio_us == 0) {
            s->t_first_audio_us = now_us();
        }
        s_bl_head += n;
        if (s_bl_head >= SIH_BACKLOG_SAMPLES) {
            s_bl_head -= SIH_BACKLOG_SAMPLES;
        }
        s_bl_count -= n;
        s->samples_sent += n;
        s->frames_sent++;
        /* Keep the ring drained while catching up on a long backlog. */
        drain_ring(s);
    }
    return true;
}

static void fail(session_t *s, const char *reason)
{
    s->failed = true;
    s->end_reason = reason;
    s->state = SESS_DONE;
}

static void ws_start(session_t *s)
{
    atomic_store(&s_ws_task_alive, true);
    const esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        atomic_store(&s_ws_task_alive, false);
        fail(s, "ws_start_failed");
        return;
    }
    s->state = SESS_WAIT_WS;
    s->deadline_us = now_us() + (int64_t)CONFIG_SIH_WS_CONNECT_TIMEOUT_MS * 1000;
}

static bool send_text(const char *text)
{
    const int len = (int)strlen(text);
    return esp_websocket_client_send_text(s_ws, text, len, pdMS_TO_TICKS(CONFIG_SIH_WS_SEND_TIMEOUT_MS)) == len;
}

/* ------------------------------------------------------------------ WebSocket */
static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *d = (const esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xTaskNotify(s_self, NET_EVT_WS_CONNECTED, eSetBits);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xTaskNotify(s_self, NET_EVT_WS_DOWN, eSetBits);
        break;
    case WEBSOCKET_EVENT_FINISH:
        atomic_store(&s_ws_task_alive, false);
        xTaskNotify(s_self, NET_EVT_WS_DOWN, eSetBits);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d != NULL) {
            ESP_LOGW(TAG, "websocket error: type %d, HTTP status %d, tls err 0x%x, errno %d",
                     (int)d->error_handle.error_type, d->error_handle.esp_ws_handshake_status_code,
                     (unsigned)d->error_handle.esp_tls_last_esp_err, d->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d != NULL && d->op_code == 0x01 && d->data_len > 0) { /* text frame from the server */
            ESP_LOGI(TAG, "server: %.*s", d->data_len, d->data_ptr);
#if CONFIG_SIH_EOS_HONOR_SERVER_SIGNAL
            if (contains(d->data_ptr, d->data_len, "end_of_speech")) {
                xTaskNotify(s_self, NET_EVT_SERVER_EOS, eSetBits);
            }
#endif
        }
        break;
    default:
        break;
    }
}

static void ws_client_init(void)
{
    esp_websocket_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.uri = CONFIG_SIH_WS_URI;
    cfg.disable_auto_reconnect = true;      /* network_task owns the connection lifecycle */
    cfg.task_core_id_set = true;
    cfg.task_core_id = SIH_CORE_NET;        /* keep all networking on core 1 */
    cfg.task_prio = SIH_PRIO_WS_CLIENT;
    cfg.task_stack = SIH_STACK_WS_CLIENT;
    cfg.task_name = "ws_client";
    cfg.buffer_size = SIH_WS_BUFFER_BYTES;  /* one max-size audio frame, no fragmentation */
    cfg.network_timeout_ms = CONFIG_SIH_WS_CONNECT_TIMEOUT_MS;
    cfg.subprotocol = NULL;
    /* Sessions last seconds. The default 10 s PING would fire right after CONNECTED
     * (the client task holds its lock while sending it) just as the pre-roll burst
     * starts, so push it out of the way. */
    cfg.ping_interval_sec = 3600;
    if (strncmp(CONFIG_SIH_WS_URI, "wss://", 6) == 0) {
#if CONFIG_SIH_WS_TLS_CRT_BUNDLE
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
#else
        cfg.cert_pem = server_cert_pem_start;   /* NUL-terminated by EMBED_TXTFILES */
        cfg.cert_len = 0;
        cfg.skip_cert_common_name_check = true; /* laptop addressed by IP; the cert itself is pinned */
#endif
    }
    s_ws = esp_websocket_client_init(&cfg);
    configASSERT(s_ws != NULL);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));
}

static void ws_teardown(session_t *s)
{
    /* Clean close only if Wi-Fi is still up: the close frame is sent with an unbounded
     * timeout inside esp_websocket_client, which could block for minutes on a dead link. */
    if (esp_websocket_client_is_connected(s_ws) && wifi_link_is_connected()) {
        if (s->state == SESS_STREAMING || s->t_ws_us != 0) {
            snprintf(s_text, sizeof(s_text),
                     "{\"type\":\"stop\",\"reason\":\"%s\",\"audio_ms\":%" PRIu32 "}",
                     s->end_reason ? s->end_reason : "unknown", s->samples_sent / SIH_SAMPLES_PER_MS);
            (void)send_text(s_text);
        }
        (void)esp_websocket_client_close(s_ws, pdMS_TO_TICKS(WS_CLOSE_TIMEOUT_MS));
    } else if (atomic_load(&s_ws_task_alive)) {
        (void)esp_websocket_client_stop(s_ws);
    }
    atomic_store(&s_ws_task_alive, false);
}

/* ------------------------------------------------------------------ session */
static void log_session(const session_t *s)
{
    const int64_t ts = s->wake.timestamp_us;
    ESP_LOGI(TAG, "session #%" PRIu32 " %s: end=%s", s->number, s->failed ? "FAILED" : "done",
             s->end_reason ? s->end_reason : "?");
    ESP_LOGI(TAG, "  T1 handoff (WAKE_CONFIRMED -> network_task) : %" PRId64 " us", s->t_rx_us - ts);
    if (s->t_wifi_us) {
        ESP_LOGI(TAG, "  Wi-Fi ready                              : +%" PRId64 " ms", (s->t_wifi_us - ts) / 1000);
    }
    if (s->t_ws_us) {
        ESP_LOGI(TAG, "  WebSocket connected                      : +%" PRId64 " ms", (s->t_ws_us - ts) / 1000);
    }
    if (s->t_first_audio_us) {
        ESP_LOGI(TAG, "  first audio byte sent                    : +%" PRId64 " ms", (s->t_first_audio_us - ts) / 1000);
    }
    ESP_LOGI(TAG, "  audio sent %" PRIu32 " ms in %" PRIu32 " frames (pre-roll %" PRIu32 " ms, post-trigger %" PRIu32
             " ms), speech heard: %s",
             s->samples_sent / SIH_SAMPLES_PER_MS, s->frames_sent, s->preroll_samples / SIH_SAMPLES_PER_MS,
             s->post_trigger_samples / SIH_SAMPLES_PER_MS, s->speech_seen ? "yes" : "no");
    ESP_LOGI(TAG, "  backlog peak %" PRIu32 " ms of %" PRIu32 " ms, dropped %" PRIu32 " samples, ring skipped %" PRIu32
             " samples",
             s->backlog_peak / SIH_SAMPLES_PER_MS, (uint32_t)(SIH_BACKLOG_SAMPLES / SIH_SAMPLES_PER_MS),
             s->backlog_dropped, s->ring_skipped);
}

static void run_session(uint32_t number)
{
    session_t *s = &s_sess;
    memset(s, 0, sizeof(*s));
    s->number = number;

    if (!pipeline_take_wake_confirmed(&s->wake)) {
        ESP_LOGW(TAG, "NET_EVT_WAKE without an event in the mailbox, ignoring");
        if (pipeline_mode() != PIPE_MODE_DETECT) {
            pipeline_resume_detection();
        }
        return;
    }
    s->t_rx_us = now_us();

    /* ---- step 1: freeze the pre-roll (time critical, before anything else) ---- */
    const ring_buffer_t *ring = pipeline_ring();
    const uint32_t wp = rb_write_pos(ring);
    int64_t lag_us = s->t_rx_us - s->wake.timestamp_us;   /* audio recorded since the trigger */
    if (lag_us < 0) {
        lag_us = 0;
    }
    uint64_t lag_samples = (uint64_t)lag_us * SIH_SAMPLES_PER_MS / 1000u;
    if (lag_samples > RING_SAFE_BACK_SAMPLES) {
        lag_samples = RING_SAFE_BACK_SAMPLES;
    }
    s->trigger_pos = wp - (uint32_t)lag_samples;
    uint32_t back = (uint32_t)lag_samples + SIH_PREROLL_SAMPLES;
    back = ((back + SIH_HOP_SAMPLES - 1u) / SIH_HOP_SAMPLES) * SIH_HOP_SAMPLES; /* stay hop aligned */
    if (back > RING_SAFE_BACK_SAMPLES) {
        back = RING_SAFE_BACK_SAMPLES;
    }
    s->read_pos = wp - back;
    s->preroll_samples = (uint32_t)rb_pos_diff(s->trigger_pos, s->read_pos);
    ulTaskNotifyValueClear(NULL, WS_EVENT_BITS | NET_EVT_AUDIO); /* stale bits from last session */
    backlog_reset();
    drain_ring(s); /* the pre-roll is now frozen in the backlog */
    seed_noise_floor(s, wp - RING_SAFE_BACK_SAMPLES, wp - back);

    /* ---- step 2: radio ---- */
    wifi_link_session_begin();
    s->state = SESS_WAIT_WIFI;
    s->deadline_us = s->t_rx_us + (int64_t)CONFIG_SIH_WIFI_CONNECT_TIMEOUT_MS * 1000;
    if (wifi_link_is_connected()) {
        s->t_wifi_us = now_us();
        ws_start(s); /* step 3 */
    }

    ESP_LOGI(TAG, "WAKE_CONFIRMED #%" PRIu32 ": confidence %.2f, handoff %" PRId64 " us, pre-roll %" PRIu32
             " ms frozen, Wi-Fi %s", number, (double)s->wake.confidence, s->t_rx_us - s->wake.timestamp_us,
             s->preroll_samples / SIH_SAMPLES_PER_MS, wifi_link_is_connected() ? "already up" : "coming up");

    /* ---- steps 3-4: event loop, woken every hop by audio_ingest_task ---- */
    while (s->state != SESS_DONE) {
        uint32_t bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, SESSION_WAIT_TICKS);
        const int64_t t = now_us();

        if (bits & NET_EVT_WIFI_RETRY) {
            wifi_link_on_retry_timer();
        }
        drain_ring(s);

        if (t - s->t_rx_us > SESSION_HARD_LIMIT_US) {
            fail(s, "session_hard_limit");
            break;
        }

        switch (s->state) {
        case SESS_WAIT_WIFI:
            if (wifi_link_is_connected()) {
                s->t_wifi_us = t;
                ws_start(s);
            } else if (t > s->deadline_us) {
                fail(s, "wifi_timeout");
            }
            break;

        case SESS_WAIT_WS:
            if (bits & NET_EVT_WS_CONNECTED) {
                s->t_ws_us = t;
                snprintf(s_text, sizeof(s_text),
                         "{\"type\":\"start\",\"session\":%" PRIu32 ",\"sample_rate\":%u,\"format\":\"s16le\","
                         "\"channels\":1,\"preroll_ms\":%" PRIu32 ",\"wake_confidence\":%.3f,"
                         "\"wake_timestamp_us\":%" PRId64 "}",
                         s->number, (unsigned)SIH_SAMPLE_RATE_HZ, s->preroll_samples / SIH_SAMPLES_PER_MS,
                         (double)s->wake.confidence, s->wake.timestamp_us);
                if (!send_text(s_text)) {
                    fail(s, "send_failed");
                    break;
                }
                s->state = SESS_STREAMING;
                if (!send_audio(s)) { /* pre-roll + backlog right away */
                    fail(s, "send_failed");
                }
            } else if (bits & NET_EVT_WS_DOWN) {
                fail(s, "ws_connect_failed");
            } else if (t > s->deadline_us) {
                fail(s, "ws_connect_timeout");
            }
            break;

        case SESS_STREAMING:
            if (bits & NET_EVT_SERVER_EOS) {
                mark_eos(s, s->read_pos, "server_end_of_speech");
            }
            if (!send_audio(s)) {
                fail(s, "send_failed");
                break;
            }
            if (all_audio_queued(s) && s_bl_count == 0) {
                s->state = SESS_DONE; /* everything up to the end point has been sent */
            } else if ((bits & NET_EVT_WS_DOWN) && !esp_websocket_client_is_connected(s_ws)) {
                fail(s, "ws_closed_by_server");
            }
            break;

        case SESS_DONE:
        default:
            break;
        }
    }

    /* ---- step 5: teardown ---- */
    pipeline_set_mode(PIPE_MODE_COOLDOWN); /* ingest stops notifying us; ring keeps filling */
    ws_teardown(s);
    wifi_link_session_end();
    log_session(s);

    /* Cooldown (DESIGN CHOICE #4 at the top of this file). Blocked, zero CPU. */
    ESP_LOGI(TAG, "cooldown %d ms", CONFIG_SIH_COOLDOWN_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SIH_COOLDOWN_MS));
    ulTaskNotifyValueClear(NULL, WS_EVENT_BITS | NET_EVT_AUDIO);
    pipeline_resume_detection();
}

/* ------------------------------------------------------------------ task */
void network_task(void *arg)
{
    (void)arg;
    pipeline_wait_start();
    s_self = xTaskGetCurrentTaskHandle();

    s_margin_lin = powf(10.0f, (float)CONFIG_SIH_EOS_SPEECH_MARGIN_DB / 10.0f);
    s_min_speech_lin = powf(10.0f, (float)CONFIG_SIH_EOS_MIN_SPEECH_DBFS / 10.0f);
    s_floor_rise = powf(10.0f, NOISE_FLOOR_RISE_DB_S * ((float)SIH_HOP_MS / 1000.0f) / 10.0f);

    wifi_link_init(s_self);
    ws_client_init();
    ESP_LOGI(TAG, "network_task ready on core %d: %s, server %s", xPortGetCoreID(), wifi_link_policy_name(),
             CONFIG_SIH_WS_URI);

    uint32_t sessions = 0;
    for (;;) {
        uint32_t bits = 0;
        /* Idle: fully blocked. Only WAKE_CONFIRMED (or a Wi-Fi link event) wakes us. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (bits & NET_EVT_WIFI_RETRY) {
            wifi_link_on_retry_timer();
        }
        if (bits & NET_EVT_WAKE) {
            run_session(++sessions);
        }
    }
}
