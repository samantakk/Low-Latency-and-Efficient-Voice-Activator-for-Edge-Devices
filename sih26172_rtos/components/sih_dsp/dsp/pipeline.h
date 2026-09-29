// pipeline.h
// Ties together framing/windowing -> FFT -> periodogram -> Mel filterbank -> PCEN.
//
// Interface boundary: this expects one FRAME_SAMPLES-length chunk of already-extracted,
// normalized ([-1,1) range) Q15.16 audio per call. Everything upstream of that --
// ring buffer management, I2S DMA, the 18-bit-in-32-bit-slot unpacking, hop-driven frame
// extraction from a continuous stream -- is Embedded C++/RTOS territory per the team
// division. This keeps the DSP sector's code testable on a laptop with synthetic frames,
// independent of firmware plumbing.

#pragma once
#include "q_format.h"
#include "framing.h"
#include "mel_filterbank.h"
#include "pcen.h"

typedef struct {
    pcen_state_t pcen_state;
} dsp_pipeline_t;

void dsp_pipeline_init(dsp_pipeline_t* pipeline);

// raw_frame: FRAME_SAMPLES (400) Q15.16 samples, NOT yet windowed (this function windows them).
// feature_out: NUM_MEL_FILTERS (40) Q15.16 values -- the PCEN feature vector for this frame
// (what the NN reads).
// mel_energies_out: optional (pass nullptr if not needed) -- the pre-PCEN Mel filterbank
// output, exposed for tier-2 VAD (see vad.cpp for why it needs pre-PCEN energies specifically).
// NOTE: raw_frame is modified in place (windowed) -- pass a scratch copy if the caller needs
// the original samples afterward.
void dsp_pipeline_process_frame(dsp_pipeline_t* pipeline, q15_16_t* raw_frame,
                                 q15_16_t* feature_out, q15_16_t* mel_energies_out = nullptr);
