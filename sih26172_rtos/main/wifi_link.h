#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Wi-Fi station management for network_task. Link events are forwarded to
 * notify_task as NET_EVT_WIFI_UP / NET_EVT_WIFI_DOWN / NET_EVT_WIFI_RETRY bits.
 */

/* One-time init (NVS, netif, event loop, Wi-Fi driver). In ASSOCIATED_PS mode this
 * also starts the radio and connects in the background. Call from network_task. */
void wifi_link_init(TaskHandle_t notify_task);

/* Streaming session starts: make sure the radio is on and a connection is under way,
 * and switch power save off for throughput. Non-blocking; wait for NET_EVT_WIFI_UP. */
void wifi_link_session_begin(void);

/* Streaming session over: back to the idle policy (modem sleep or radio off). */
void wifi_link_session_end(void);

/* Handle NET_EVT_WIFI_RETRY (back-off timer expired): try to connect again. */
void wifi_link_on_retry_timer(void);

bool wifi_link_is_connected(void);

const char *wifi_link_policy_name(void);
