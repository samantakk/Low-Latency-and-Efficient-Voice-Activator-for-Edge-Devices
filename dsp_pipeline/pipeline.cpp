// pipeline.cpp
#include "pipeline.h"
#include "fft_fixed.h"

void dsp_pipeline_init(dsp_pipeline_t* pipeline) {
    pcen_init(&pipeline->pcen_state);
}

void dsp_pipeline_process_frame(dsp_pipeline_t* pipeline, q15_16_t* raw_frame,
                                 q15_16_t* feature_out, q15_16_t* mel_energies_out) {
    // 1. Window (in place)
    apply_hamming_window(raw_frame, FRAME_SAMPLES);

    // 2. Zero-pad 400 -> 512 and load into complex buffer
    static complex_q15_16_t fft_buffer[FFT_SIZE]; // static: keeps this off the call stack,
                                                   // matches embedded practice of fixed buffers
    fft_load_real_frame(raw_frame, FRAME_SAMPLES, fft_buffer);

    // 3. FFT (bit-reverse reorder, then in-place DIT butterfly stages)
    fft_bit_reverse_reorder(fft_buffer);
    fft_fixed_512(fft_buffer);

    // 4. Periodogram (257 unique bins, with the stage-scaling correction applied)
    static q15_16_t periodogram[NUM_FFT_BINS];
    compute_periodogram(fft_buffer, periodogram);

    // 5. Mel filterbank (257 bins -> 40 channel energies)
    static q15_16_t mel_energies[NUM_MEL_FILTERS];
    apply_mel_filterbank(periodogram, mel_energies);

    // Exposed to the caller (tier-2 VAD wants these BEFORE PCEN compresses the cross-channel
    // dynamic range -- see vad.cpp's design note on why SFM belongs here, not on PCEN output).
    if (mel_energies_out) {
        for (int c = 0; c < NUM_MEL_FILTERS; c++) mel_energies_out[c] = mel_energies[c];
    }

    // 6. PCEN (stateful across frames via pipeline->pcen_state) -- this is the NN's input,
    // not tier 2's.
    pcen_process_frame(&pipeline->pcen_state, mel_energies, feature_out);
}
