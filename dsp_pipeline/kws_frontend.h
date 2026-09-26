// kws_frontend.h
// Ties together: tier-1 gate -> (maybe) DSP pipeline -> tier-2 gate -> feature matrix ->
// NN-invocation decision. This is the per-hop (every 10ms) entry point the RTOS task calls.
//
// NN invocation cadence (confirmed with NN team): every NN_INVOKE_EVERY_N_HOPS (5) hops,
// invoke if ANY of those 5 hops passed both gates -- not just the specific 5th one. This
// catches cases where a word's onset (a quiet consonant) doesn't clear the gates on the one
// hop that happens to land on the boundary, but a hop right after it clearly does.
//
// NOTE for whoever owns the NN training pipeline: this "any of 5" logic should match how
// training windows were sampled/labeled -- if training assumed the older "only the specific
// 5th hop matters" cadence, there's a train/deploy mismatch worth checking.

#pragma once
#include "q_format.h"
#include "framing.h"
#include "pipeline.h"
#include "vad.h"
#include "feature_matrix.h"

#define NN_INVOKE_EVERY_N_HOPS 5 // 5 x 10ms hop = 50ms, per your cadence

typedef struct {
    dsp_pipeline_t dsp;
    vad_tier1_state_t vad1;
    vad_tier2_state_t vad2;
    feature_matrix_t matrix;
    int hop_counter;
    bool any_voice_hop_this_window; // OR'd across the current 5-hop window, reset after each check
} kws_frontend_t;

typedef struct {
    bool ran_dsp;       // did the FFT/Mel/PCEN pipeline actually run this hop?
    bool tier1_passed;
    bool tier2_passed;  // only meaningful if ran_dsp is true
    bool invoke_nn;      // should the caller now run NN inference on the matrix?
} kws_hop_result_t;

void kws_frontend_init(kws_frontend_t* fe);

// raw_frame: FRAME_SAMPLES (400) Q15.16 samples, freshly read from the ring buffer this hop,
// NOT yet windowed (kws_frontend windows it internally via the DSP pipeline, only if tier 1
// passes -- so the "skip the expensive stuff on silence" saving is real, not just on paper).
// raw_frame is modified in place if the DSP pipeline runs (same caveat as
// dsp_pipeline_process_frame -- pass a scratch copy if the caller needs the originals after).
kws_hop_result_t kws_frontend_process_hop(kws_frontend_t* fe, q15_16_t* raw_frame);
