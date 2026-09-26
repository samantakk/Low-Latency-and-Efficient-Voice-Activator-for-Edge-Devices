# DSP Pipeline — KWS Feature Extraction + Two-Tier VAD Gating (Sambit's sector)

Full per-hop flow (matches your team's design):
```
I2S -> ring buffer -> [every 10ms: read 25ms window]
  -> Tier 1 gate (cheap, raw audio)
       fail -> duplicate last feature vector into the NN's matrix, done (no DSP work)
       pass -> FFT -> periodogram -> Mel filterbank -> PCEN
                 -> Tier 2 gate (accurate, pre-PCEN Mel energies)
                      -> append fresh PCEN vector to matrix either way
                      -> every 5th hop (50ms): if tier1 AND tier2 both passed, invoke NN
```

## Files

| File | What it does |
|---|---|
| `q_format.h` | Q15.16 fixed-point core |
| `fixed_math.h` | Integer sqrt, log2/exp2/pow (bit-extraction) |
| `fft_fixed.h/.cpp` | 512-pt radix-2 DIT FFT |
| `framing.h/.cpp` | Frame/hop constants, Hamming window |
| `mel_filterbank.h/.cpp` | 40 sparse Mel filters, periodogram computation |
| `pcen.h/.cpp` | PCEN endpoint — the NN's actual input |
| `pipeline.h/.cpp` | FFT->periodogram->Mel->PCEN, now also exposes pre-PCEN Mel energies |
| `vad.h/.cpp` | **New.** Tier 1 (raw-audio energy) + Tier 2 (spectral flatness) gates |
| `feature_matrix.h/.cpp` | **New.** Ring-buffer "2D matrix" the NN reads, with duplicate-on-tier1-fail logic |
| `kws_frontend.h/.cpp` | **New.** Orchestrates gates + pipeline + matrix + NN-invocation cadence, one call per hop |
| `test_pipeline.cpp` | Feature-extraction accuracy tests (fixed vs. float reference) |
| `test_frontend.cpp` | **New.** Gating behavior test — silence/voice/silence, checks DSP and NN invocation counts |

## Build & run

```
g++ -O2 -Wall -std=c++17 -o test_pipeline \
  test_pipeline.cpp pipeline.cpp pcen.cpp mel_filterbank.cpp framing.cpp fft_fixed.cpp
g++ -O2 -Wall -std=c++17 -o test_frontend \
  test_frontend.cpp kws_frontend.cpp feature_matrix.cpp vad.cpp pipeline.cpp pcen.cpp \
  mel_filterbank.cpp framing.cpp fft_fixed.cpp
```

`test_frontend` result on a synthetic silence→voice→silence sequence: DSP pipeline ran on 0%
of silent hops and 100% of voice hops (33.3% overall); NN invoked on 20 of 300 hops (6.7%) —
exactly matching the every-5th-voice-hop cadence. That's the actual CPU saving the two-tier
design buys, not just the theoretical claim.

## Two design findings from testing (not just implementing to spec)

1. **Tier 2 cannot use an online-adapting threshold the way Tier 1 does.** Tier 1 sees every
   hop (including real silence), so its adaptive noise floor learns correctly. Tier 2 only
   ever runs on hops Tier 1 already passed — so an adapting "quiet baseline" there only ever
   learns from already-loud frames, including real voice the first time it shows up. That's a
   self-defeating loop (the threshold chases whatever it just saw), and it showed up
   immediately as Tier 2 never triggering at all in `test_frontend`, even on obvious "voice."
   Fixed with a flat, fixed threshold instead — which also happens to be the more principled
   choice, since spectral flatness is already scale-invariant and shouldn't need per-session
   adaptation the way raw energy does.

2. **Tier 2 must run on pre-PCEN Mel energies, not the finished PCEN vector.** PCEN's whole
   job is to compress each channel's own dynamic range over time — exactly the cross-channel
   "peakiness" a flatness measure needs to tell voice from noise. Measuring flatness after
   PCEN measures a signal PCEN has already partly flattened by design. `pipeline.cpp` now
   exposes the pre-PCEN Mel energies via an optional out-parameter specifically for this.

## Calibrate before trusting this on real hardware

All of these are placeholders tuned only to make synthetic test signals behave sensibly:

- `ENERGY_GATE_MULT_Q`, `ENERGY_FLOOR_ADAPT_*` in `vad.cpp` (Tier 1)
- `SFM_VOICE_THRESHOLD_Q` in `vad.cpp` (Tier 2) — needs real recorded speech AND real
  recorded non-speech-but-louder-than-ambient sounds (door closing, chair scraping — exactly
  what Tier 1 alone would let through) to set properly
- `PCEN_ENERGY_FLOOR`, `PERIODOGRAM_GAIN_SHIFT` — see prior notes, unchanged
- `NUM_CONTEXT_HOPS` in `feature_matrix.h` — the NN's expected input time-context length;
  this is genuinely the NN team's call, not a DSP-sector decision
- **Open assumption to confirm with your team**: `kws_frontend.cpp` invokes the NN when the
  hop counter reaches 5 *and that specific hop* passed both gates — not "any voice hop in the
  last 5." If the intended design is "invoke as soon as voice has been sustained for 5 hops"
  instead, that's a different (still easy) state machine — worth a quick check before this
  goes into firmware.
