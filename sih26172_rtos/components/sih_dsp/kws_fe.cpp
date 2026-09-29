/* kws_fe.cpp - extern "C" bridge, see include/kws_fe.h. Contains no DSP logic. */
#include "kws_fe.h"

#include "dsp/kws_frontend.h" /* pulls in q_format.h, framing.h, pipeline.h (the DSP one), ... */

/* ---- contract with the RTOS side, checked at build time ---- */
static_assert(KWS_FE_SAMPLE_RATE_HZ == SAMPLE_RATE_HZ, "sample rate differs from framing.h");
static_assert(KWS_FE_FRAME_SAMPLES == FRAME_SAMPLES, "window length differs from framing.h");
static_assert(KWS_FE_HOP_SAMPLES == HOP_SAMPLES, "hop length differs from framing.h");
static_assert(KWS_FE_NUM_MEL == NUM_MEL_FILTERS, "Mel band count differs from mel_filterbank.h");
static_assert(KWS_FE_CONTEXT_HOPS == NUM_CONTEXT_HOPS, "context length differs from feature_matrix.h");
static_assert(KWS_FE_INVOKE_EVERY_HOPS == NN_INVOKE_EVERY_N_HOPS, "NN cadence differs from kws_frontend.h");
static_assert(sizeof(q15_16_t) == sizeof(int32_t), "Q15.16 must be a 32-bit word");
static_assert(sizeof(kws_fe_matrix_t) == sizeof(q15_16_t[NUM_CONTEXT_HOPS][NUM_MEL_FILTERS]),
              "matrix layout differs from feature_matrix.h");

/* The one and only frontend instance (owned by dsp_task). Static: no malloc. */
static kws_frontend_t s_fe;

/* Work buffers that dsp_pipeline_process_frame() keeps as function-local statics
 * (pipeline.cpp): 512 complex FFT points + 257-bin periodogram + 40 Mel energies. */
static constexpr size_t kPipelineStaticBytes =
    FFT_SIZE * sizeof(complex_q15_16_t) + NUM_FFT_BINS * sizeof(q15_16_t) + NUM_MEL_FILTERS * sizeof(q15_16_t);

extern "C" {

void kws_fe_init(void)
{
    kws_frontend_init(&s_fe);
}

kws_fe_hop_t kws_fe_process_hop(int32_t *frame)
{
    const kws_hop_result_t r = kws_frontend_process_hop(&s_fe, reinterpret_cast<q15_16_t *>(frame));
    kws_fe_hop_t out;
    out.ran_dsp = r.ran_dsp;
    out.tier1_passed = r.tier1_passed;
    out.tier2_passed = r.tier2_passed;
    out.invoke_nn = r.invoke_nn;
    return out;
}

void kws_fe_snapshot(kws_fe_matrix_t *out)
{
    feature_matrix_get_ordered(&s_fe.matrix, reinterpret_cast<q15_16_t(*)[NUM_MEL_FILTERS]>(*out));
}

size_t kws_fe_state_bytes(void)
{
    return sizeof(s_fe) + kPipelineStaticBytes;
}

} /* extern "C" */
