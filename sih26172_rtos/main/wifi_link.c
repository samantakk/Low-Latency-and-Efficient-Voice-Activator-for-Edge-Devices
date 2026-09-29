/*
 * wifi_link.c - Wi-Fi station state machine used by network_task.
 *
 * DESIGN CHOICE (open question #1, Wi-Fi power state between detections):
 * default = stay ASSOCIATED in max modem-sleep (WIFI_PS_MAX_MODEM, listen interval
 * CONFIG_SIH_WIFI_LISTEN_INTERVAL) rather than full power-down (esp_wifi_stop).
 * See implementation.md "Open questions". Selectable in menuconfig
 * (SIH26172 -> Wi-Fi -> "Wi-Fi state between detections") so both can be measured.
 *
 *   Why associated by default: the pre-roll and everything the user says after the
 *   wake word has to be buffered in RAM until the first byte can be sent. With full
 *   power-down every trigger pays scan + auth/assoc + DHCP (typically 1-3 s, worse on
 *   busy networks) before TLS even starts, which (a) directly inflates the T1 -> first
 *   byte latency the team is measured on and (b) needs a much bigger streaming backlog
 *   (48 KB covers 1.5 s). Staying associated leaves only TCP + TLS + WebSocket upgrade on
 *   the trigger path.
 *   Cost: the radio wakes for DTIM beacons every listen interval (a few mA average
 *   instead of ~0 when off) and the Wi-Fi driver adds small periodic CPU load; watch
 *   the idle % reported by stats_task to see it on your AP.
 *   While a stream is active power save is switched OFF (WIFI_PS_NONE), because in
 *   modem sleep incoming TCP ACKs are delayed to beacon time and throughput collapses.
 *
 *   With SIH_WIFI_IDLE_POWER_OFF the radio is started on WAKE_CONFIRMED and stopped at
 *   teardown; the last AP channel is remembered to shorten the scan.
 *
 * Reconnect policy (both modes, only while a connection is wanted): 3 immediate retries,
 * then exponential back-off 1 s, 2 s, 4 s ... 30 s using a static FreeRTOS one-shot
 * timer (no polling). The timer only notifies network_task, which calls esp_wifi_connect.
 */
#include "wifi_link.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "pipeline.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define FAST_RETRIES      3u
#define BACKOFF_MIN_MS    1000u
#define BACKOFF_MAX_MS    30000u

static TaskHandle_t s_notify;
static _Atomic bool s_connected;
static _Atomic bool s_want_connected;
static _Atomic uint32_t s_retry_count;
static StaticTimer_t s_retry_timer_buf;
static TimerHandle_t s_retry_timer;

const char *wifi_link_policy_name(void)
{
#if CONFIG_SIH_WIFI_IDLE_POWER_OFF
    return "power-off between detections";
#else
    return "associated + max modem-sleep between detections";
#endif
}

bool wifi_link_is_connected(void)
{
    return atomic_load(&s_connected);
}

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    /* Runs in the FreeRTOS timer service task: just hand over to network_task. */
    xTaskNotify(s_notify, NET_EVT_WIFI_RETRY, eSetBits);
}

static void connect_now(void)
{
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* No DISCONNECTED event follows a refused call, so arm the back-off timer
         * ourselves, otherwise nothing would ever retry. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s, retrying in %u ms", esp_err_to_name(err), (unsigned)BACKOFF_MIN_MS);
        if (atomic_load(&s_want_connected)) {
            (void)xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(BACKOFF_MIN_MS), 0);
        }
    }
}

static void schedule_retry(void)
{
    const uint32_t n = atomic_fetch_add(&s_retry_count, 1u);
    if (n < FAST_RETRIES) {
        connect_now();
        return;
    }
    uint32_t shift = n - FAST_RETRIES;
    if (shift > 5u) {
        shift = 5u;
    }
    uint32_t delay_ms = BACKOFF_MIN_MS << shift;
    if (delay_ms > BACKOFF_MAX_MS) {
        delay_ms = BACKOFF_MAX_MS;
    }
    ESP_LOGW(TAG, "reconnect in %" PRIu32 " ms (attempt %" PRIu32 ")", delay_ms, n + 1u);
    /* xTimerChangePeriod also starts a dormant timer. */
    if (xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(delay_ms), 0) != pdPASS) {
        ESP_LOGE(TAG, "could not arm the reconnect timer");
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (atomic_load(&s_want_connected)) {
                connect_now();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)data;
            ESP_LOGI(TAG, "associated (channel %d), waiting for IP", e->channel);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
            if (atomic_exchange(&s_connected, false)) {
                xTaskNotify(s_notify, NET_EVT_WIFI_DOWN, eSetBits);
            }
            if (atomic_load(&s_want_connected)) {
                ESP_LOGW(TAG, "disconnected, reason %d", e->reason);
                schedule_retry();
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        if (!atomic_load(&s_want_connected)) {
            return; /* late event after the radio was switched off (power-off policy) */
        }
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        atomic_store(&s_retry_count, 0u);
        xTimerStop(s_retry_timer, 0);
        atomic_store(&s_connected, true);
        xTaskNotify(s_notify, NET_EVT_WIFI_UP, eSetBits);
    }
}

void wifi_link_init(TaskHandle_t notify_task)
{
    s_notify = notify_task;

    esp_err_t err = nvs_flash_init(); /* the PHY calibration data lives in NVS */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));

    s_retry_timer = xTimerCreateStatic("wifi_retry", pdMS_TO_TICKS(BACKOFF_MIN_MS), pdFALSE, NULL,
                                       retry_timer_cb, &s_retry_timer_buf);
    configASSERT(s_retry_timer != NULL);

    /* Credentials only in RAM: avoids NVS flash writes (flash writes stall the caches). */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid, CONFIG_SIH_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_SIH_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (strlen(CONFIG_SIH_WIFI_PASSWORD) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
#if CONFIG_SIH_WIFI_IDLE_ASSOCIATED_PS
    wc.sta.listen_interval = CONFIG_SIH_WIFI_LISTEN_INTERVAL;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

#if CONFIG_SIH_WIFI_IDLE_ASSOCIATED_PS
    atomic_store(&s_want_connected, true);
    ESP_ERROR_CHECK(esp_wifi_start()); /* connects from the STA_START event */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    ESP_LOGI(TAG, "policy: %s (listen interval %d), connecting to \"%s\"", wifi_link_policy_name(),
             CONFIG_SIH_WIFI_LISTEN_INTERVAL, CONFIG_SIH_WIFI_SSID);
#else
    atomic_store(&s_want_connected, false);
    ESP_LOGI(TAG, "policy: %s, radio stays off until WAKE_CONFIRMED (SSID \"%s\")",
             wifi_link_policy_name(), CONFIG_SIH_WIFI_SSID);
#endif
}

void wifi_link_session_begin(void)
{
#if CONFIG_SIH_WIFI_IDLE_ASSOCIATED_PS
    /* Full-power radio while streaming. */
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE): %s", esp_err_to_name(err));
    }
    if (!wifi_link_is_connected() && xTimerIsTimerActive(s_retry_timer) != pdFALSE) {
        /* We were waiting out a back-off: retry right now instead. If no timer is armed
         * an attempt is already in progress, and we just wait for it. */
        xTimerStop(s_retry_timer, 0);
        atomic_store(&s_retry_count, 0u);
        connect_now();
    }
#else
    atomic_store(&s_connected, false); /* never trust a flag left over from the last session */
    atomic_store(&s_retry_count, 0u);
    atomic_store(&s_want_connected, true);
    esp_err_t err = esp_wifi_start(); /* STA_START event -> esp_wifi_connect() */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE): %s", esp_err_to_name(err));
    }
#endif
}

void wifi_link_session_end(void)
{
#if CONFIG_SIH_WIFI_IDLE_ASSOCIATED_PS
    /* Back to the associated low-power idle state. */
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(MAX_MODEM): %s", esp_err_to_name(err));
    }
#else
    /* Remember the AP channel so the next start scans that channel first. */
    wifi_ap_record_t ap;
    const bool have_ap = wifi_link_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

    atomic_store(&s_want_connected, false);
    xTimerStop(s_retry_timer, 0);
    (void)esp_wifi_disconnect();
    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    atomic_store(&s_connected, false);

    if (have_ap) {
        wifi_config_t wc;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            wc.sta.channel = ap.primary;
            (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
    }
    ESP_LOGI(TAG, "radio powered down");
#endif
}

void wifi_link_on_retry_timer(void)
{
    if (atomic_load(&s_want_connected) && !wifi_link_is_connected()) {
        connect_now();
    }
}
