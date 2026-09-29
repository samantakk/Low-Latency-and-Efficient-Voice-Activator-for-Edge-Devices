/*
 * kws_fe.h - C interface to the DSP team's C++ frontend (kws_frontend.h).
 *
 * The RTOS side is C and the DSP sector is C++, and the two sets of headers must never be
 * included in the same translation unit (q_format.h vs audio_format.h, dsp pipeline.h vs
 * main/pipeline.h). This header is the only thing the RTOS code sees of the DSP sector.
 *
 * Ownership: the frontend state (PCEN, both VAD tiers, the 100 x 40 feature matrix, the
 * hop counter) is one file-static object in kws_fe.cpp. It is owned exclusively by
 * dsp_task: only dsp_task may call anything below except kws_fe_state_bytes().
 *
 * Every constant below is checked with static_assert against the DSP headers in
 * kws_fe.cpp, so a change on the DSP side breaks the build instead of the firmware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_FE_SAMPLE_RATE_HZ     16000  /* framing.h SAMPLE_RATE_HZ */
#define KWS_FE_FRAME_SAMPLES      400    /* framing.h FRAME_SAMPLES: window handed to process_hop */
#define KWS_FE_HOP_SAMPLES        160    /* framing.h HOP_SAMPLES */
#define KWS_FE_NUM_MEL            40     /* mel_filterbank.h NUM_MEL_FILTERS */
#define KWS_FE_CONTEXT_HOPS       100    /* feature_matrix.h NUM_CONTEXT_HOPS */
#define KWS_FE_INVOKE_EVERY_HOPS  5      /* kws_frontend.h NN_INVOKE_EVERY_N_HOPS */

/* The NN's input as the DSP sector produces it: [oldest hop ... newest hop][mel band],
 * Q15.16 (int32_t, 1.0 == 65536). Same layout as feature_matrix_get_ordered(). */
typedef int32_t kws_fe_matrix_t[KWS_FE_CONTEXT_HOPS][KWS_FE_NUM_MEL];

/* Mirror of kws_hop_result_t. */
typedef struct {
    bool ran_dsp;        /* tier 1 passed: FFT/Mel/PCEN ran this hop */
    bool tier1_passed;
    bool tier2_passed;   /* only meaningful when ran_dsp */
    bool invoke_nn;      /* true once per 5-hop window in which any hop passed both tiers */
} kws_fe_hop_t;

void kws_fe_init(void);

/*
 * One call per 10 ms hop. `frame` = the newest KWS_FE_FRAME_SAMPLES normalized Q15.16 samples
 * (straight from the ring buffer, un-windowed). It is MODIFIED IN PLACE (windowed) when
 * tier 1 passes, so pass a scratch copy.
 */
kws_fe_hop_t kws_fe_process_hop(int32_t *frame);

/* Copy the feature matrix, oldest hop first (feature_matrix_get_ordered). Call only after
 * process_hop reported invoke_nn: the snapshot is what nn_task reads on the other core. */
void kws_fe_snapshot(kws_fe_matrix_t *out);

/* Static RAM used by the frontend, including the DSP pipeline's static work buffers. */
size_t kws_fe_state_bytes(void);

#ifdef __cplusplus
}
#endif
