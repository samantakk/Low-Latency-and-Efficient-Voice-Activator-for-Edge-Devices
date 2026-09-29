#pragma once

#include <stdint.h>

/*
 * network_task: fully blocked while idle. On WAKE_CONFIRMED it freezes the pre-roll,
 * brings Wi-Fi up, opens the WebSocket, streams pre-roll + live audio as binary
 * frames of raw PCM s16le, tears everything down at end of speech, runs the cooldown
 * and resumes DSP/NN.
 */
void network_task(void *arg);

/* Static RAM owned by the streaming module (backlog FIFO, buffers, session state). */
uint32_t net_stream_static_bytes(void);
