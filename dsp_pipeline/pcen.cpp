// pcen.cpp
#include "pcen.h"
#include "fixed_math.h"

// Wang et al. PCEN paper defaults, as Q15.16 constants (all computed offline, no runtime float).
#define PCEN_S            1638    // s     = 0.025   (smoothing coefficient)
#define PCEN_ONE_MINUS_S  63898   // 1-s   = 0.975
#define PCEN_ALPHA        64225   // alpha = 0.98
#define PCEN_DELTA        131072  // delta = 2.0
#define PCEN_R            32768   // r     = 0.5
#define PCEN_DELTA_POW_R  92682   // delta^r = 2.0^0.5 -- precomputed once, since both are fixed
#define PCEN_EPS          1       // smallest representable positive Q15.16 value (1/65536).
                                   // The paper's eps=1e-6 would round to 0 at this resolution --
                                   // this is the practical equivalent: just enough to keep
                                   // (eps + M) off zero so log2 in pow_q15_16 never sees 0.

// PLACEHOLDER -- must be recalibrated from a real recorded noise floor (see pcen.h comment).
// In the gained periodogram/Mel-energy units (PERIODOGRAM_GAIN_SHIFT in mel_filterbank.cpp),
// this is real value 0.05, i.e. raw ~3277. Chosen only to be comfortably above the handful-of-
// raw-units level where the ratio math misbehaves on this synthetic test signal -- not derived
// from any actual mic measurement.
#define PCEN_ENERGY_FLOOR 3277

static inline q15_16_t pcen_apply_floor(q15_16_t e) {
    return (e < PCEN_ENERGY_FLOOR) ? PCEN_ENERGY_FLOOR : e;
}

void pcen_init(pcen_state_t* state) {
    for (int c = 0; c < NUM_MEL_FILTERS; c++) {
        state->M[c] = 0;
    }
    state->initialized = false;
}

void pcen_process_frame(pcen_state_t* state, const q15_16_t* mel_energies, q15_16_t* pcen_out) {
    for (int c = 0; c < NUM_MEL_FILTERS; c++) {
        // Floor applied to E before it touches anything else -- both the ratio's numerator
        // directly, and (via the IIR below) its own smoothed history M a few frames later.
        // Flooring only one side would just relocate the ill-conditioned edge, not remove it.
        q15_16_t E = pcen_apply_floor(mel_energies[c]);

        // M_t = (1-s)*M_{t-1} + s*E_t  -- one-pole IIR smoother, per channel, across frames.
        q15_16_t M_prev = state->M[c];
        q15_16_t M_new = add_q15_16(
            mul_q15_16(PCEN_ONE_MINUS_S, M_prev),
            mul_q15_16(PCEN_S, E)
        );
        state->M[c] = M_new;

        // denom = (eps + M_t)^alpha
        q15_16_t denom = pow_q15_16(add_q15_16(PCEN_EPS, M_new), PCEN_ALPHA);

        // normalized = E_t / denom
        q15_16_t normalized = div_q15_16(E, denom);

        // compressed = (normalized + delta)^r - delta^r
        q15_16_t compressed = pow_q15_16(add_q15_16(normalized, PCEN_DELTA), PCEN_R);
        pcen_out[c] = sub_q15_16(compressed, PCEN_DELTA_POW_R);
    }
    state->initialized = true;
}
