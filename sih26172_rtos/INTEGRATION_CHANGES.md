# RTOS <-> DSP / NN integration: what was incompatible and what changed

DSP (`dsp_pipeline.zip`) and NN (`kws_nn.zip`) code is untouched. The DSP sources are copied
verbatim into `components/sih_dsp/dsp/`; the NN model header verbatim into `components/sih_nn/`.

| # | Incompatibility in the RTOS code | Fix |
|---|---|---|
| 1 | RTOS is C, DSP/NN are C++; none of it was in the build | New components `sih_dsp` (+ `kws_fe.h/.cpp` extern "C" bridge) and `sih_nn` (+ `kws_nn.h/.cpp`); `main` REQUIRES both. DSP headers are private so `dsp/pipeline.h` never shadows `main/pipeline.h` |
| 2 | `audio_format.h` and `q_format.h` both define `Q15_16_ONE` (different text) and `q15_16_to_float()` (redefinition error in one TU) | `audio_format.h` now defines the same tokens as `q_format.h`; its float helper is `sih_q15_16_to_float()`. Verified: original fails, fixed compiles |
| 3 | `dsp_task` counted hops itself and notified the NN unconditionally every `CONFIG_SIH_NN_INVOKE_EVERY_HOPS` (5) hops, bypassing tier-1/tier-2 gating and the "any of last 5 hops" rule | `dsp_task` calls `kws_fe_process_hop()` per hop and obeys `invoke_nn`. Kconfig option removed; cadence constant is `KWS_FE_INVOKE_EVERY_HOPS` (static_assert-ed against `kws_frontend.h`) |
| 4 | Feature matrix was a 100x40 **int8** placeholder; real one is **Q15.16 int32** (16 KB) | Placeholder removed; RAM report uses real sizes (`kws_fe_state_bytes()`) |
| 5 | NN reads the feature matrix from core 1 while `dsp_task` appends to it on core 0 (data race; the NN team's file ran both in one task) | Snapshot handoff: `pipeline_snapshot_acquire/publish/get/release`. One atomic ownership flag, no lock, `dsp_task` never blocks (drops and counts if NN is busy) |
| 6 | NN needs int8 input quantized with the model's own scale/zero-point | Done in `kws_nn_load_input()`, same math as the NN team's file |
| 7 | NN file has its own `app_main`, I2S ISR, semaphore, ring, `KWS_Event`, and a second 1.5 s cooldown | Not built (kept in `reference/`). RTOS owns those. `nn_task` emits `wake_confirmed_t` (timestamp in us, taken just before the handoff). NN bouncer hit-counter reset on resume instead of a second cooldown |
| 8 | `nn_task` stack 3 KB, `dsp_task` 3 KB | 8 KB (TFLM Invoke; same as the NN team's task) and 4 KB. Trim using the `MEM` stack lines |
| 9 | Warm-up length and Mel/hop constants duplicated in `app_config.h` | Now taken from `kws_fe.h`; `_Static_assert`s tie RTOS hop/window/rate to the DSP's |
| 10 | Tensor arena size was a Kconfig placeholder (0) | Real 80 KB from the NN file (`KWS_NN_TENSOR_ARENA_BYTES`); Kconfig options `SIH_NN_TENSOR_ARENA_BYTES`, `SIH_NN_INVOKE_EVERY_HOPS`, `SIH_TRIGGER_AUTO_PERIOD_S` removed (auto trigger was counted in ungated invocations, no longer meaningful) |

Renamed: `dsp_stub.*` -> `dsp_task.*`, `nn_stub.*` -> `nn_task.*` (they are no longer stubs).
Stats now also print DSP-gated %, snapshots skipped, NN inference count and max confidence.

## Verified here (host)
- DSP + `kws_fe` bridge compiled with g++ and driven from C the way `dsp_task` does: 296/398 hops gated off, NN invoked only during the voice segment (21 times, 0 outside), snapshot fully populated, all `static_assert`s hold.
- `audio_format.h` + `q_format.h` in one C++ TU.
- Ring buffer host tests: 71 checks, 0 failures.
- `pipeline.c dsp_task.c nn_task.c main.c sys_stats.c` pass `-fsyntax-only -Wall -Wextra` against mock ESP-IDF headers.

## NOT verified (needs ESP-IDF / hardware)
- A real `idf.py build`. `espressif/esp-tflite-micro` version `^1.3.0` in `components/sih_nn/idf_component.yml` is a guess; pin the one matching the model.
- Runtime behavior, timing, and the combined RAM figure (check the boot log against 256 KB; the model's arena is a placeholder in the NN file).

## Open items for the DSP/NN teams (not changed)
- `kws_model_data.h` puts an *initialized* array in `.ext_ram.bss`. That section is normally NOLOAD, so the model bytes may not be present at runtime (`kws_nn_init` would then log a schema-version failure). Drop the section attribute (model in flash) or load it into PSRAM explicitly.
- After streaming + cooldown the frontend's `last_vector` still holds the keyword's final frame, and tier-1 silence hops duplicate it into the matrix. `implementation.md` says not to reset; if testing shows false triggers after a session, re-run `feature_matrix_init()` on resume.
