// vad.h
// Two gates, matching your team's full pipeline design:
//
//   Tier 1 (before the DSP pipeline runs at all): cheap short-term energy vs. an adaptive
//   noise floor, on RAW time-domain samples. Its only job is "is there anything louder than
//   typical ambient here at all" -- deliberately loose/permissive. This is what keeps the
//   FFT/Mel/PCEN pipeline from running every single 10ms hop during idle silence, which is
//   the dominant cost that would otherwise blow the <10% CPU budget.
//
//   Tier 2 (after PCEN, before the NN): spectral flatness measure (SFM) on the finished PCEN
//   vector -- geometric-mean/arithmetic-mean across the 40 channels. Broadband noise has a
//   flat spectrum (SFM near 1); voiced speech is peaky/structured (SFM well below 1, since
//   formants concentrate energy in specific bands). This is the more accurate, more expensive
//   (but still nowhere near NN-inference-expensive) check, and it's the one that decides
//   whether the NN actually gets invoked.
//
// Both gates are adaptive (track their own "what does quiet look like" baseline via an IIR,
// same pattern as PCEN's own M-smoother) rather than fixed magic thresholds, because a fixed
// threshold picked without real recorded audio would be closer to a guess than a calibration.
// The multiplier constants below are still placeholders pending that real-audio calibration --
// see the TODO comments in vad.cpp.

#pragma once
#include "q_format.h"
#include "framing.h"
#include "mel_filterbank.h"
#include <cstdint>
#include <cstddef>

// ---- Tier 1: raw-audio energy gate ----

typedef struct {
    q15_16_t noise_floor_energy; // adaptive baseline, updated only on frames that don't trigger
    bool initialized;
} vad_tier1_state_t;

void vad_tier1_init(vad_tier1_state_t* state);

// raw_frame: FRAME_SAMPLES (400) Q15.16 samples, NOT yet windowed (tier 1 runs on the raw
// ring-buffer read, before framing.cpp's Hamming window is ever applied).
// zcr_out (optional, pass nullptr to skip): zero-crossing count, exposed for tuning/telemetry --
// not currently part of the gate decision itself (see vad.cpp for why).
bool vad_tier1_check(vad_tier1_state_t* state, const q15_16_t* raw_frame, int* zcr_out);

// ---- Tier 2: PCEN spectral-flatness gate ----

typedef struct {
    q15_16_t noise_floor_sfm; // adaptive baseline flatness, updated only on non-triggering frames
    bool initialized;
} vad_tier2_state_t;

void vad_tier2_init(vad_tier2_state_t* state);

// mel_energies: NUM_MEL_FILTERS (40) Q15.16 values -- the PRE-PCEN Mel filterbank output for
// this hop (see vad.cpp for why this needs to be pre-PCEN, not the finished PCEN vector).
bool vad_tier2_check(vad_tier2_state_t* state, const q15_16_t* mel_energies);
