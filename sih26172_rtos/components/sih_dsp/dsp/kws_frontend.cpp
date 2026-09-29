// kws_frontend.cpp
#include "kws_frontend.h"

void kws_frontend_init(kws_frontend_t* fe) {
    dsp_pipeline_init(&fe->dsp);
    vad_tier1_init(&fe->vad1);
    vad_tier2_init(&fe->vad2);
    feature_matrix_init(&fe->matrix);
    fe->hop_counter = 0;
    fe->any_voice_hop_this_window = false;
}

kws_hop_result_t kws_frontend_process_hop(kws_frontend_t* fe, q15_16_t* raw_frame) {
    kws_hop_result_t result = {false, false, false, false};

    result.tier1_passed = vad_tier1_check(&fe->vad1, raw_frame, nullptr);

    if (!result.tier1_passed) {
        // Cheapest path: no FFT, no Mel filterbank, no PCEN. Duplicate the last known feature
        // vector into the matrix so the NN's input sequence has no discontinuity -- a gap or
        // a zero-vector here would look like a sharp transient to the NN, which is exactly
        // the kind of artifact that causes false activations.
        if (fe->matrix.has_last_vector) {
            feature_matrix_append(&fe->matrix, fe->matrix.last_vector);
        } else {
            // Very first hop ever, nothing to duplicate yet -- append silence.
            q15_16_t zeros[NUM_MEL_FILTERS] = {0};
            feature_matrix_append(&fe->matrix, zeros);
        }
    } else {
        // Tier 1 passed -- do the real work.
        q15_16_t feature_vector[NUM_MEL_FILTERS];
        q15_16_t mel_energies[NUM_MEL_FILTERS];
        dsp_pipeline_process_frame(&fe->dsp, raw_frame, feature_vector, mel_energies);

        result.tier2_passed = vad_tier2_check(&fe->vad2, mel_energies);

        // Freshly computed vector goes in either way (tier 1 already told us this hop is
        // worth the DSP cost) -- tier 2 only decides whether the NN gets a look, not whether
        // this hop's real spectral content makes it into the matrix.
        feature_matrix_append(&fe->matrix, feature_vector);
    }

    result.ran_dsp = result.tier1_passed;

    // OR this hop's gate result into the running window -- "any of 5", not "just the 5th".
    fe->any_voice_hop_this_window = fe->any_voice_hop_this_window ||
                                     (result.tier1_passed && result.tier2_passed);

    fe->hop_counter++;
    if (fe->hop_counter >= NN_INVOKE_EVERY_N_HOPS) {
        fe->hop_counter = 0;
        result.invoke_nn = fe->any_voice_hop_this_window;
        fe->any_voice_hop_this_window = false; // reset for the next 5-hop window
    }

    return result;
}
