// vad.cpp
#include "vad.h"
#include "fixed_math.h"

// ============================================================================
// Tier 1: short-term energy vs. adaptive noise floor
// ============================================================================

// PLACEHOLDER -- needs real recorded ambient noise to calibrate properly (same caveat as
// PCEN_ENERGY_FLOOR: only real hardware tells you what "quiet" actually measures as).
// Frame is flagged as voice-candidate if energy > noise_floor * ENERGY_GATE_MULT.
#define ENERGY_GATE_MULT_Q      (3 * Q15_16_ONE)      // 3.0x the noise floor
// Noise floor only adapts when the current frame is already at/below a modest margin above
// the existing floor -- this stops real speech from ever dragging the "quiet" baseline upward.
#define ENERGY_FLOOR_ADAPT_CEILING_Q (2 * Q15_16_ONE) // only adapt if energy < 2x current floor
#define ENERGY_FLOOR_ADAPT_RATE_Q    6554             // 0.1 -- IIR rate, slower than PCEN's 0.025
                                                        // is intentional: PCEN follows fast per-
                                                        // channel dynamics, this just tracks a
                                                        // slowly-drifting room noise level

static q15_16_t compute_frame_energy(const q15_16_t* frame) {
    // Mean squared amplitude over the frame, computed in one pass, full 64-bit precision
    // (no intermediate mul_q15_16 truncation -- same lesson as the periodogram fix).
    int64_t sum_sq = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        int64_t s = frame[i];
        sum_sq += s * s;
    }
    int64_t mean_sq = sum_sq / FRAME_SAMPLES;   // still carries 2 factors of 65536
    int64_t energy_q15_16 = mean_sq >> Q15_16_SHIFT; // correct back to a single Q15.16 scale
    if (energy_q15_16 > INT32_MAX) energy_q15_16 = INT32_MAX;
    return (q15_16_t)energy_q15_16;
}

static int compute_zcr(const q15_16_t* frame) {
    int crossings = 0;
    for (int i = 1; i < FRAME_SAMPLES; i++) {
        bool prev_neg = frame[i - 1] < 0;
        bool cur_neg = frame[i] < 0;
        if (prev_neg != cur_neg) crossings++;
    }
    return crossings;
}

void vad_tier1_init(vad_tier1_state_t* state) {
    state->noise_floor_energy = 0;
    state->initialized = false;
}

bool vad_tier1_check(vad_tier1_state_t* state, const q15_16_t* raw_frame, int* zcr_out) {
    q15_16_t energy = compute_frame_energy(raw_frame);
    if (zcr_out) *zcr_out = compute_zcr(raw_frame);

    if (!state->initialized) {
        // Cold start: assume the first frame is representative of ambient noise. If the
        // device happens to power on mid-speech this will be wrong for one frame -- tier 2
        // and the NN downstream are the real safety net, this only needs to be roughly right.
        state->noise_floor_energy = energy;
        state->initialized = true;
        return false; // don't trigger on the seeding frame itself
    }

    q15_16_t threshold = mul_q15_16(state->noise_floor_energy, ENERGY_GATE_MULT_Q);
    bool triggered = energy > threshold;

    q15_16_t adapt_ceiling = mul_q15_16(state->noise_floor_energy, ENERGY_FLOOR_ADAPT_CEILING_Q);
    if (energy < adapt_ceiling) {
        // Only adapt when we're not clearly looking at speech -- IIR toward current energy.
        q15_16_t one_minus_rate = sub_q15_16(Q15_16_ONE, ENERGY_FLOOR_ADAPT_RATE_Q);
        state->noise_floor_energy = add_q15_16(
            mul_q15_16(one_minus_rate, state->noise_floor_energy),
            mul_q15_16(ENERGY_FLOOR_ADAPT_RATE_Q, energy)
        );
    }

    return triggered;
}

// ============================================================================
// Tier 2: spectral flatness measure (SFM) on the pre-PCEN Mel energies
// ============================================================================
//
// IMPORTANT: this runs on Mel filterbank output, NOT the PCEN vector, despite PCEN being
// "the feature vector" the NN eventually sees. PCEN's whole job is to compress each channel's
// own dynamic range over time (that's what the alpha/r exponents do) -- which is exactly the
// cross-channel peakiness SFM needs in order to tell voice from noise. Measuring flatness
// after PCEN measures a signal PCEN has already partly flattened by design.
//
// (This was caught by testing, not foresight: an earlier version ran SFM on the PCEN vector,
// and on a synthetic three-tone "voice" signal the SFM barely moved between the noise and
// voice segments -- PCEN's compression was suppressing the very contrast the gate needed.)
//
// This also does NOT use an online-adapting threshold, and that's deliberate too: SFM is a
// ratio (geometric mean / arithmetic mean), so it's already invariant to uniform scaling of
// the energy vector -- unlike tier 1's raw frame energy, which genuinely depends on mic
// gain/room level and needs to track a drifting baseline.

#define SFM_VOICE_THRESHOLD_Q    39322   // PLACEHOLDER (0.6) -- trigger if SFM < 0.6.
                                          // Needs real recorded speech + real recorded
                                          // non-speech-but-louder-than-ambient sounds (a door
                                          // closing, a chair scraping -- exactly what tier 1
                                          // alone would let through) to set properly.
#define MEL_ENERGY_MIN_FOR_LOG   1       // guard against log2(0); smallest representable value

void vad_tier2_init(vad_tier2_state_t* state) {
    state->noise_floor_sfm = Q15_16_ONE; // retained in the struct for now; unused by the fixed-
    state->initialized = true;           // threshold check below (see design note above)
}

// Spectral Flatness Measure = geometric_mean(x) / arithmetic_mean(x), computed on Mel energies.
// Geometric mean via logs: exp2( mean(log2(x_i)) ) -- avoids a 40-term product that would
// overflow, and reuses the same log2/exp2_q15_16 primitives PCEN already needs.
static q15_16_t compute_sfm(const q15_16_t* mel_energies) {
    q15_16_t log2_sum = 0;
    int64_t arith_sum = 0;

    for (int c = 0; c < NUM_MEL_FILTERS; c++) {
        q15_16_t v = mel_energies[c];
        if (v < MEL_ENERGY_MIN_FOR_LOG) v = MEL_ENERGY_MIN_FOR_LOG;
        log2_sum = add_q15_16(log2_sum, log2_q15_16(v));
        arith_sum += v;
    }

    q15_16_t log2_mean = (q15_16_t)(log2_sum / NUM_MEL_FILTERS);
    q15_16_t geometric_mean = exp2_q15_16(log2_mean);
    q15_16_t arithmetic_mean = (q15_16_t)(arith_sum / NUM_MEL_FILTERS);

    if (arithmetic_mean <= 0) return Q15_16_ONE; // degenerate all-zero case reads as "flat"
    return div_q15_16(geometric_mean, arithmetic_mean);
}

bool vad_tier2_check(vad_tier2_state_t* state, const q15_16_t* mel_energies) {
    (void)state; // kept in the interface for future use; not needed by the fixed-threshold check
    q15_16_t sfm = compute_sfm(mel_energies);
    return sfm < SFM_VOICE_THRESHOLD_Q;
}
