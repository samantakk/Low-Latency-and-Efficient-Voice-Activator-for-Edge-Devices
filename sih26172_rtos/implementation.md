# SIH26172 — RTOS / Audio Ingestion / Networking Subsystem: Implementation Plan

## Scope

This document covers the systems half of the SIH26172 wake-word pipeline: audio capture via
I2S/DMA, the RTOS task/priority architecture, the ring buffer, and the networking layer that
streams audio to a remote ASR on detection. It explicitly does NOT cover the DSP feature
extraction (FFT/Mel/PCEN) or the NN model itself — those are a separate team's deliverable.
This code integrates with them at two defined interfaces (see "Interfaces" below).

Target hardware: ESP32-S3 (dual-core), ESP-IDF + FreeRTOS.
Hard constraints: RAM < 256KB total, idle CPU < 10%, static allocation only (no malloc/new).

## Task architecture

| Task              | Core          | Priority           | Wakes on                                      | Blocks on                                   |
|-------------------|---------------|---------------------|------------------------------------------------|----------------------------------------------|
| audio_ingest_task | 0 (pinned)    | Highest             | I2S DMA-complete ISR                           | xTaskNotifyGiveFromISR from DMA ISR          |
| dsp_task          | 0 (pinned)    | High                | Notification from audio_ingest_task, per 10ms hop | Task notification                        |
| nn_task           | 1 (pinned)    | Medium              | Notification from dsp_task, gated by Tier-1 VAD | Task notification, ~every 50ms when ungated |
| network_task      | 1 (pinned)    | Medium-low, normally blocked | WAKE_CONFIRMED from nn_task           | Semaphore/notification, fully blocked at idle |

Rationale for core split: audio capture + DSP windowing need deterministic 10ms timing (Core 0).
NN invocation + networking are bursty and can tolerate jitter (Core 1). Pinning avoids relying
on preemption alone to protect the 10ms hop from a slow NN invocation.

Use xTaskCreatePinnedToCore for all four tasks.

## Ring buffer

- Circular buffer in SRAM, static allocation, sized to hold at least 25ms of audio (one DSP
  window) plus margin — not just one 10ms hop, since dsp_task reads backward across multiple
  hops of history on every read.
- Contents: normalized Q15.16 samples. NOT raw I2S words. See "Interfaces" below — the I2S
  word conversion (18-bit sample inside a 24-bit slot inside a 32-bit word, sign-extension +
  normalization) happens in audio_ingest_task, immediately after DMA delivers a buffer, before
  the sample is written into the ring buffer.
- Writer: audio_ingest_task only.
- Readers: dsp_task (windowed reads, 25ms window / 10ms hop, must handle wraparound
  transparently since a window can span the wrap point) and network_task (drains the
  pre-roll on WAKE_CONFIRMED, then continues reading live samples as they arrive).
- Size the DMA buffer itself to exactly one hop's worth of samples (10ms @ 16kHz = 160 samples)
  so that each DMA-complete interrupt naturally corresponds to one hop tick — this gives
  push-based, event-driven 10ms cadence with no separate polling timer needed.

## DSP/NN state ownership

- Bundle all persistent DSP/VAD state into one static struct (e.g. dsp_state_t): PCEN
  per-channel smoothing values, Tier-0/Tier-1 adaptive thresholds, the sliding ~1s x 100-frame
  feature matrix, ring buffer read position.
- This struct is owned exclusively by dsp_task — no other task reads or writes it, so no mutex
  is needed. Keep it as a static local to the task function or passed via pvParameters, not a
  global visible elsewhere in the codebase.
- This state must persist across every hop call and never be reset except at explicit
  cooldown/re-init points (see "Cooldown" below).

## Memory rules (apply everywhere in this subsystem)

- No malloc / new anywhere.
- xTaskCreateStatic for every task (fixed, pre-declared stack sizes).
- xQueueCreateStatic for any queues.
- Static semaphore/notification buffers.
- Confirm at integration time that this subsystem's static footprint (ring buffer + dsp_state_t
  + task stacks) plus the NN team's Tensor Arena together stay under the 256KB budget — get
  their Arena size and report both numbers side by side.

## Interfaces (confirm explicitly with the DSP/NN team before parallel implementation)

1. **Ring buffer content format.** This subsystem writes normalized Q15.16 samples, not raw
   I2S words. dsp_task should never need to handle hardware bit-packing.

2. **WAKE_CONFIRMED{confidence, timestamp} struct.** Emitted once by nn_task, consumed by
   network_task via notification. The timestamp is the project's T1 latency checkpoint
   (edge-confirms-keyword) — the handoff code between receiving this and unblocking
   network_task must not add avoidable delay, since this gap is now part of a metric the whole
   team is measured on.

3. **DSP/NN suspend during streaming.** Per the reference doc, RTOS "bypasses the DSP/NN
   pipeline entirely" once WAKE_CONFIRMED fires — DSP/NN must not run concurrently with cloud
   streaming. Implement via either (a) a shared flag nn_task checks each invocation before
   running, or (b) explicit vTaskSuspend on dsp_task/nn_task from the handoff code, with
   vTaskResume at the end of cooldown. Pick one and document it — this is a real design choice,
   not a detail to leave implicit.

4. **Cooldown.** After streaming ends (WebSocket closed, radio powered down), RTOS owns a
   defined cooldown period before dsp_task/nn_task resume. Prevents immediate re-triggering on
   trailing audio. Start with ~1-2s, tune against real testing later. dsp_state_t should NOT be
   reset during this — history should remain valid once resumed, unless testing shows otherwise.

## Streaming pipeline (on WAKE_CONFIRMED)

1. network_task unblocks (notification from nn_task).
2. Wi-Fi power-on: esp_wifi brings up the radio, connects to the local router. (If keeping the
   radio in a lighter always-associated idle state is chosen instead of full power-down between
   detections, implement that state machine here — this is an open tradeoff, see "Open
   questions".)
3. Secure connection: resolve server address, TLS handshake (mbedtls, hardware crypto
   accelerator offload where available), establish a persistent wss:// WebSocket
   (esp_websocket_client).
4. Data transmission: drain and send the pre-roll audio frozen from the ring buffer at trigger
   time, then seamlessly continue streaming live audio as it keeps arriving via the DMA
   ping-pong buffers. Send as binary WebSocket frames (raw PCM), not JSON-wrapped.
5. Teardown: once the user stops speaking (end-of-speech signal — define this: could be a
   fixed max duration, a VAD-based cutoff, or a server-side signal), close the WebSocket, power
   down the Wi-Fi radio, begin cooldown, then let RTOS drop the CPU back into Tickless Idle.

## Power management

- Enable CONFIG_FREERTOS_USE_TICKLESS_IDLE and configure esp_pm_configure() for automatic
  light-sleep.
- Instrument with ulTaskGetIdleRunTimePercent() from the very first working build — this is
  the only way to verify the <10% budget empirically rather than assuming it.
- No periodic polling anywhere in this subsystem. Every wakeup must be traceable to a real
  hardware interrupt (DMA-complete) or an inter-task notification triggered by one.

## Build and integration order

1. Ring buffer, standalone, unit-tested for wraparound correctness under continuous writes.
2. I2S + DMA -> audio_ingest_task -> ring buffer, real mic hardware, ISR-safe notification per
   10ms chunk. Verify the I2S word conversion produces correct Q15.16 samples.
3. Tickless idle + power measurement. Get ulTaskGetIdleRunTimePercent() reporting a real
   baseline BEFORE adding any DSP/NN/network load.
4. Stub dsp_task: no real FFT/PCEN yet, just consume notifications and log timing, to validate
   10ms cadence holds under the two-core pinning.
5. Stub trigger for network_task: a GPIO button press or timer standing in for WAKE_CONFIRMED,
   so networking can be built independent of the NN team's readiness.
6. WebSocket client + a local open-source ASR server stub (e.g. Vosk or Whisper-based, running
   on a laptop) — validate the full streaming path end-to-end using the stub trigger from
   step 5.
7. Wire in the real DSP pipeline once the ring buffer content-format interface (item 1 above)
   is confirmed with the DSP team — replace the stub from step 4.
8. Wire in the real NN task once the model is ready — replace the stub trigger from step 5 with
   the real WAKE_CONFIRMED handoff, including the suspend/cooldown logic (items 3-4 above).
9. Full pipeline measurement: idle CPU%, RAM footprint (combined with NN team's Tensor Arena),
   and end-to-end latency from WAKE_CONFIRMED timestamp to first byte sent over WebSocket.

Steps 1-6 have no dependency on the DSP/NN team's code being ready. This is the critical path
to start immediately, in parallel with their model work.

## Open questions to resolve with the team before/during implementation

- Wi-Fi power state between detections: full power-down (saves power, costs connection latency
  each time) vs. lighter always-associated idle (opposite tradeoff). Affects the T1 latency
  metric directly — needs a decision, not an assumption.
- End-of-speech detection for streaming teardown: fixed duration, local VAD-based cutoff, or a
  signal from the ASR server itself? Not specified in the reference doc.
- DSP/NN suspend mechanism (flag vs. vTaskSuspend) — pick one, document the choice in code
  comments at the handoff point.
- Cooldown duration — start at 1-2s, needs empirical tuning once the full pipeline is testable.
