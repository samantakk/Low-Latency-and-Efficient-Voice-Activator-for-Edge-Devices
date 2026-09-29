// pcen.h
// Per-Channel Energy Normalization. Per your technical-decisions: you implement this as a
// fixed-constant transform now; if the NN team later learns (s, alpha, delta, r) via
// backpropagation, they hand back final constants and only PCEN_S/ALPHA/DELTA/R below change
// — no other file in this pipeline needs to know.
//
//   M_t       = (1-s)*M_{t-1} + s*E_t                         (per-channel IIR smoother)
//   PCEN_t    = (E_t / (eps + M_t)^alpha + delta)^r - delta^r
//
// M_t is *state* -- one value per Mel channel, carried across frames. That statefulness is
// exactly why this needs a context struct instead of being a stateless function like the FFT
// or filterbank: PCEN only means something as a function of the recent past, not one frame
// in isolation.
//
// Constants below are the Wang et al. PCEN paper's defaults, NOT tuned for this task yet --
// treat them as a working placeholder, not a final answer.
//
// PCEN_ENERGY_FLOOR (see pcen.cpp) exists for a reason worth knowing before you touch it:
// PCEN's E/(eps+M)^alpha is ill-conditioned whenever BOTH E and its own smoothed history M
// are near zero -- dividing "almost nothing" by "even less" spikes unpredictably, regardless
// of precision. A pure synthetic test tone genuinely has zero energy in most Mel channels
// (no mic ever does -- real self-noise + ambient noise floor every channel), which is exactly
// what exposed this. The floor keeps E and M from both collapsing toward zero together. Its
// value here is a placeholder -- it needs to be set from a real recorded noise floor once you
// have INMP441 hardware, which is also the "real negative/background-noise data" your own
// risk assessment already flagged as necessary for the false-activation-rate risk.

#pragma once
#include "q_format.h"
#include "mel_filterbank.h"
#include <cstdint>

typedef struct {
    q15_16_t M[NUM_MEL_FILTERS]; // smoothed energy state, one per Mel channel
    bool initialized;
} pcen_state_t;

void pcen_init(pcen_state_t* state);

// mel_energies: NUM_MEL_FILTERS Q15.16 values (this frame's filterbank output, E_t)
// pcen_out: NUM_MEL_FILTERS Q15.16 values (this frame's feature vector)
// Updates state->M in place (this is the "streaming" part -- call once per frame, in order).
void pcen_process_frame(pcen_state_t* state, const q15_16_t* mel_energies, q15_16_t* pcen_out);
