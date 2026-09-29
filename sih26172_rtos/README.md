# SIH26172: RTOS, audio ingestion and networking subsystem (ESP32-S3)

This is a complete ESP-IDF project for the systems half of the SIH26172 wake-word pipeline, built from `implementation.md` (included in this folder). It covers:

- microphone capture over I2S/DMA
- the four FreeRTOS tasks
- the ring buffer
- power management
- streaming audio to a server over WebSocket after a detection

The DSP frontend (`dsp_pipeline.zip`) and the NN model (`kws_nn.zip`) are now integrated. See **INTEGRATION_CHANGES.md** for what was incompatible and how it was fixed.

---

## 1. What was built, and where the stubs are

| Part | File(s) | Status |
|---|---|---|
| Ring buffer (Q15.16, lock-free, 1 writer / many readers, wraparound) | `components/sih_audio_core/` | **Real**, unit tested on PC |
| I2S word to Q15.16 conversion | `components/sih_audio_core/include/audio_format.h` | **Real**, unit tested |
| `audio_ingest_task` (I2S + DMA, 1 DMA buffer = 1 hop of 160 samples, ISR notification) | `main/audio_ingest.c` | **Real** |
| `dsp_task` (25 ms window every 10 ms hop into the DSP frontend) | `main/dsp_task.c`, `components/sih_dsp/` | **Real** (host-tested bridge; not run on hardware) |
| `nn_task` (TFLite Micro model, emits WAKE_CONFIRMED) | `main/nn_task.c`, `components/sih_nn/` | **Real**; BOOT button kept as a manual trigger. Not built or run here (needs ESP-IDF) |
| `network_task` (Wi-Fi, WebSocket, pre-roll + live streaming, teardown, cooldown) | `main/net_stream.c`, `main/wifi_link.c` | **Real** |
| Mode switching, WAKE_CONFIRMED handoff, task start barrier | `main/pipeline.c` | **Real** |
| Power management (DFS, auto light sleep, tickless idle) and CPU idle % | `main/main.c`, `main/sys_stats.c`, `sdkconfig.defaults` | **Real** |
| Laptop test server (saves WAV, timing report, optional Vosk ASR) | `tools/asr_server.py` | **Real**, tested |
| Fake device to test the server without hardware | `tools/fake_device.py` | **Real**, tested |

### How the DSP and NN sectors are wired in

- **DSP:** `components/sih_dsp/dsp/` holds the DSP team's C++ sources, unmodified. `kws_fe.h` is the only header the C code sees. `dsp_task` calls `kws_fe_process_hop()` once per hop and the frontend decides gating and NN cadence.
- **NN:** `components/sih_nn/kws_nn.cpp` is the model init / quantize / invoke / bouncer code from `esp32_kws_firmware.cpp` (kept in `reference/`). `nn_task` calls it.
- **Handoff:** `dsp_task` copies the feature matrix into a shared snapshot (`pipeline_snapshot_*`) and `nn_task` reads that, never the live matrix.
- **Interfaces:** ring buffer format `audio_format.h`; WAKE_CONFIRMED `wake_event.h`; DSP `kws_fe.h`; NN `kws_nn.h`.

### Task layout

| Task | Core | Priority | Wakes on |
|---|---|---|---|
| audio_ingest_task | 0 | 21 (highest of ours) | I2S DMA-complete interrupt, every 10 ms |
| dsp_task | 0 | 19 | notification from ingest, every hop (only in DETECT mode) |
| nn_task | 1 | 10 | snapshot from dsp_task when the DSP gates open (at most every 5 hops), or the button |
| network_task | 1 | 7 | WAKE_CONFIRMED; then every hop while streaming; blocked when idle |
| stats_task (diagnostics, extra) | 1 | 2 | notification from ingest every 5 s |

All tasks use `xTaskCreateStaticPinnedToCore`. This is ESP-IDF's call that is both "xTaskCreateStatic" and "xTaskCreatePinnedToCore". The queue, event group and timer are all static. Nothing in the project calls `malloc`/`new`. ESP-IDF's own drivers (Wi-Fi, lwIP, TLS, I2S driver) allocate internally, and that heap use is printed live on the `MEM` line.

No task polls. Every wakeup comes from the DMA interrupt, a notification caused by it, a Wi-Fi event, or the button.

### The four open questions: what was chosen

Each choice is marked `DESIGN CHOICE` in the code. All of them can be changed in menuconfig.

| # | Question | Choice | Where |
|---|---|---|---|
| 1 | Wi-Fi between detections | Stay **associated in max modem-sleep** (listen interval 3). Power save goes off while streaming and back on after. Full power-down is one menuconfig switch away, so both can be measured. | `main/wifi_link.c` (top) |
| 2 | End of speech | **Hybrid, whichever comes first.** 800 ms trailing silence after speech (simple energy detector); no speech within 3 s = false trigger; 8 s hard cap; or the server sends `end_of_speech`. Audio up to the end point is always sent before closing. | `main/net_stream.c` (top) |
| 3 | DSP/NN suspend | **Cooperative mode flag, not `vTaskSuspend`.** Ingest stops notifying dsp while streaming, and dsp/nn park at the top of their loop. vTaskSuspend could freeze a task while it holds the log or libc lock and deadlock others. | `main/pipeline.c` (top) |
| 4 | Cooldown | **1500 ms**, counted after the WebSocket is closed and the radio is idle. `dsp_state_t` is not reset. After resuming, dsp waits about 1 s of fresh audio before calling nn again, so the old keyword frames cannot re-fire. | `main/net_stream.c`, `main/dsp_task.c` |

---

## 2. Setup, build and flash

### Hardware

- An ESP32-S3 board (DevKitC-1 or similar) and an I2S MEMS mic.
- The defaults are for the **SPH0645** (18-bit). For an **INMP441** set "Valid sample bits" to 24.

| Mic pin | ESP32-S3 (default, changeable in menuconfig) |
|---|---|
| BCLK / SCK | GPIO 4 |
| LRCL / WS | GPIO 5 |
| DOUT / SD | GPIO 6 |
| SEL / L/R | GND (left channel) |
| 3V / VDD, GND | 3.3 V, GND |

The stub trigger is the **BOOT button (GPIO 0)**.

### Software

This needs ESP-IDF **v5.3 or newer**. The first build downloads `esp_websocket_client` from the Espressif component registry automatically, so it needs internet.

```bash
# 1. Load ESP-IDF into the shell (path depends on where you installed it)
. $HOME/esp/esp-idf/export.sh

# 2. Go to the project and select the chip (only needed once)
cd sih26172_rtos
idf.py set-target esp32s3

# 3. Set Wi-Fi name/password, server URI, mic pins, etc.
idf.py menuconfig
#    -> "SIH26172 wake-word subsystem"
#         Wi-Fi -> SSID, password
#         Streaming server -> WebSocket URI, e.g. ws://192.168.1.50:8765/stream
#                             (use the address the laptop server prints at startup)
#         Microphone -> pins, valid sample bits (18 = SPH0645, 24 = INMP441)

# 4. Build, flash and watch the log (replace the port)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor      # macOS: /dev/cu.usbserial-XXXX or /dev/cu.usbmodemXXXX
                                          # Windows: COM5
# Exit the monitor with Ctrl+]
```

Your Wi-Fi password is stored in the generated `sdkconfig`. It is listed in `.gitignore`, so do not commit it.

### Laptop server (for streaming tests)

```bash
cd tools
pip install -r requirements.txt
python3 asr_server.py                              # prints the ws:// URL to put in menuconfig
python3 fake_device.py                             # optional: test the server with no board
python3 asr_server.py --server-eos-after-ms 1500   # tests the "server says stop" path
```

To add real speech recognition:

1. `pip install vosk`.
2. Download a model (for example `vosk-model-small-en-us-0.15`).
3. Run `python3 asr_server.py --vosk-model PATH --server-eos`.

Each session is saved in `tools/sessions/` as a WAV file plus a JSON file. Play the WAV to hear exactly what the board sent.

To test **wss:// (TLS)** against the laptop:

1. Run `./make_cert.sh <laptop-ip>`.
2. In menuconfig, set "Server certificate check" to **Pinned certificate** and the URI to `wss://<laptop-ip>:8765/stream`.
3. Rebuild and flash.
4. Run `python3 asr_server.py --certfile certs/server_cert.pem --keyfile certs/server_key.pem`.

### Host unit tests (no board needed)

```bash
./test/host/run_tests.sh      # expect: "71 checks, 0 failures / ALL TESTS PASSED"
```

---

## 3. Bring-up checklist: what to check at each task boundary

Do these in order. Everything is on the serial monitor. The `stats` block prints every 5 s. Numbers below are what healthy output should look like. The exact values depend on your board and room.

### Boundary 1: I2S/DMA -> audio_ingest_task -> ring buffer

At boot:

```
I (...) ingest: I2S RX started: 16000 Hz, 32-bit mono (left slot), 18 valid bits, DMA 6 x 160 frames (640 bytes) = one hop per interrupt ...
```

Every 5 s:

```
I (...) stats: I2S  hops 1500 (+500) dma_q_ovf 0 rd_err 0 empty_wakes ... | level peak -38.2 dBFS rms -55.0 dBFS dc -0.0120
```

- `+500` hops per 5 s means exactly 100 DMA interrupts per second, one per 10 ms.
- `dma_q_ovf 0` and `rd_err 0` must stay at 0.
- **Level check (this verifies the Q15.16 conversion):**
  - Quiet room: rms roughly -50 to -65 dBFS.
  - Talking or clapping near the mic: rms rises by 20 dB or more and the peak goes toward -10 dBFS.
  - If the level is stuck at -120 dBFS (all zeros), or near 0 dBFS all the time, check the wiring, the L/R pin and the "valid sample bits" setting.
  - A small constant `dc` value (SPH0645 has a DC offset) is normal.
- Final proof: stream once (boundary 4) and listen to the WAV. It should sound clean, at the right pitch and speed.

### Boundary 2: audio_ingest_task -> dsp_task (10 ms cadence under 2-core pinning)

```
I (...) stats: DSP  hops +500 wakes +500 period min/avg/max 9800/10000/10300 us | proc avg/max 30/60 us | max hops/wake 1 | wrap windows +3 | rd_err 0 resync 0
```

- `hops +500` must match ingest's `+500`.
- `period avg` should be about 10000 us. min/max within a few hundred us is healthy.
- `max hops/wake 1` means dsp never fell behind. 2 once in a while is fine; growing numbers are not.
- `wrap windows` > 0 shows windows that spanned the physical end of the ring were read correctly.
- `rd_err 0`.

CPU budget line (build step 3, the baseline before real DSP/NN):

```
I (...) stats: CPU  idle% interval C0 98.1 C1 99.0 | since boot C0 97 C1 98 | load C0 1.9% C1 1.0% (budget <10%: OK)
```

Record these idle numbers now. They are the baseline before real DSP/NN load.

### Boundary 3: dsp_task -> nn_task -> WAKE_CONFIRMED -> network_task

```
I (...) stats: NN   notified +100 invocations +100 | WAKE_CONFIRMED total 0
```

- `+100` per 5 s = every 50 ms. "notified" and "invocations" should match.
- Press **BOOT**. You should see:
  ```
  I (...) net: WAKE_CONFIRMED #1: confidence 1.00, handoff 60 us, pre-roll 300 ms frozen, Wi-Fi already up
  ```
- **handoff** is the T1 handoff delay (WAKE_CONFIRMED timestamp to network_task running). It should be tens to a few hundred microseconds.
- While streaming, the stats line shows `mode=STREAM`, DSP `hops +0`, NN `invocations +0`. That means DSP/NN are really bypassed.

### Boundary 4: network_task streaming, teardown, cooldown, resume

Start `tools/asr_server.py` first. Then press BOOT and say something.

```
I (...) net: session #1 done: end=trailing_silence
I (...) net:   T1 handoff (WAKE_CONFIRMED -> network_task) : 58 us
I (...) net:   Wi-Fi ready                              : +0 ms
I (...) net:   WebSocket connected                      : +45 ms
I (...) net:   first audio byte sent                    : +47 ms
I (...) net:   audio sent 2640 ms in 60 frames (pre-roll 300 ms, post-trigger 2340 ms), speech heard: yes
I (...) net:   backlog peak 400 ms of 1500 ms, dropped 0 samples, ring skipped 0 samples
I (...) net: cooldown 1500 ms
I (...) pipeline: mode -> DETECT (DSP/NN resumed, dsp_state_t preserved)
```

- **first audio byte sent** is the end-to-end number for spec step 9.
- `dropped 0` and `ring skipped 0` mean no audio was lost.
- The session should end on its own, about 0.8 s after you stop talking.
- After cooldown, the DSP line shows `resync 1` once, and NN invocations come back after about 1 s (the warm-up).
- On the laptop, the server prints a matching report, including "live rate ... 1.00x real time". It also saves the WAV. **Listen to it:** it should start about 300 ms before you pressed the button and contain everything you said.
- Press BOOT a few more times to confirm repeated sessions work and heap `min ever` on the MEM line stays stable.

If `end=wifi_timeout`: check the SSID and password. If `end=ws_connect_failed` or `ws_connect_timeout`: check the URI, that the laptop is on the same network, and the laptop firewall (port 8765).

### Memory report (spec: compare with the NN team's Tensor Arena)

At boot, `main` prints this subsystem's static RAM, line by line, next to the Tensor Arena size. Current defaults come to about 110 KB of static RAM:

- ring 32 KB
- backlog 48 KB
- dsp_state 6 KB
- stacks

Enter the NN team's number in menuconfig ("NN team's Tensor Arena size") to get the combined total against the 256 KB budget. The `MEM` line shows the heap that ESP-IDF's Wi-Fi/TLS use, and the free stack of every task. Use the stack numbers to trim stack sizes later.

---

## 4. Things to know

- **Light sleep never actually happens while listening.** Auto light sleep and tickless idle are configured as the spec asks. But the I2S driver holds a power-management lock while the mic is running (always, for a wake word), so the chip stays awake and the CPU floor is 80 MHz. The `<10%` budget is CPU load, and stats_task measures it.
- **RAM tuning:**
  - The streaming backlog (1.5 s) must cover pre-roll + connection time. If a session log shows `dropped > 0`, raise it in menuconfig (32 bytes per ms).
  - With the Wi-Fi "power-off" policy, connections take longer. Plan on 3000 ms or more.
  - To give ESP-IDF more heap, the Wi-Fi driver's IRAM speed options are turned off in `sdkconfig.defaults`. Our stream is only 32 KB/s.
- **The end-of-speech detector is deliberately simple.** It is an energy threshold. Tune `Speech margin`, `Trailing silence` and `No speech timeout` in menuconfig against real rooms. The server-side `end_of_speech` signal from Vosk usually works better.
- **Changing `sdkconfig.defaults` after the first build** has no effect until you delete `sdkconfig` (or run `idf.py menuconfig` and change the option there).
- **Wire format:**
  - Audio is binary WebSocket frames of raw PCM s16le, 16 kHz, mono.
  - The only JSON is one small `start` text frame before the audio and one `stop` frame after it.
  - The server can send back `{"type":"end_of_speech"}` and transcripts.

## 5. Folder layout

```
sih26172_rtos/
  README.md                  this file
  implementation.md          the spec this was built from
  CMakeLists.txt             ESP-IDF project file
  sdkconfig.defaults         power management, run-time stats, core pinning, TLS, RAM options
  main/
    Kconfig.projbuild        all project settings (menuconfig -> "SIH26172 wake-word subsystem")
    idf_component.yml        pulls espressif/esp_websocket_client ^1.7.0
    app_config.h             timing constants, task table, compile-time checks
    main.c                   app_main: power, task creation, RAM report
    pipeline.c/.h            mode flag (DESIGN CHOICE #3), handles, WAKE_CONFIRMED handoff
    wake_event.h             WAKE_CONFIRMED struct (interface #2)
    audio_ingest.c/.h        audio_ingest_task
    dsp_task.c/.h            dsp_task (calls the DSP frontend)
    nn_task.c/.h             nn_task (runs the NN model)
    net_stream.c/.h          network_task, streaming, end of speech (#2), cooldown (#4)
    wifi_link.c/.h           Wi-Fi state machine (DESIGN CHOICE #1)
    sys_stats.c/.h           stats_task: idle %, timing, memory
    certs/                   put server_cert.pem here for pinned wss:// (tools/make_cert.sh)
  components/sih_audio_core/ ring buffer + sample format (plain C, host testable)
  components/sih_dsp/        DSP team sources (dsp/, unmodified) + kws_fe C bridge
  components/sih_nn/         NN model, TFLM glue, reference/ = NN team original
  test/host/                 run_tests.sh + ring buffer / conversion unit tests
  tools/                     asr_server.py, fake_device.py, make_cert.sh, requirements.txt
```
