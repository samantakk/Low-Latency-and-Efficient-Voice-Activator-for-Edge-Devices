/*
 * kws_nn.h - C interface to the NN team's TFLite Micro model.
 *
 * Only nn_task calls this (it owns the interpreter; TFLM is not thread safe).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "kws_fe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tensor arena: the NN team's figure (esp32_kws_firmware.cpp). Their file marks it a
 * PLACEHOLDER: re-measure with RecordingMicroAllocator and change it here. */
#define KWS_NN_TENSOR_ARENA_BYTES  (80u * 1024u)

typedef struct {
    float confidence;   /* sigmoid output of the last inference, 0..1 */
    bool triggered;     /* bouncer confirmed the keyword on this inference */
} kws_nn_result_t;

/* Build the interpreter, allocate tensors, verify input shape/type. Logs the reason and
 * returns false on failure. Call once from nn_task before the first inference. */
bool kws_nn_init(void);

/* Quantize the Q15.16 feature matrix into the model's int8 input tensor, using the
 * scale / zero-point read from the model itself. Cheap (4000 values), no Invoke. */
void kws_nn_load_input(const kws_fe_matrix_t *matrix);

/* Invoke() on the loaded input, then run the bouncer. Returns false if Invoke failed. */
bool kws_nn_run(kws_nn_result_t *result);

/* Forget bouncer history (called when detection resumes after streaming + cooldown). */
void kws_nn_reset(void);

size_t kws_nn_arena_bytes(void);

#ifdef __cplusplus
}
#endif
