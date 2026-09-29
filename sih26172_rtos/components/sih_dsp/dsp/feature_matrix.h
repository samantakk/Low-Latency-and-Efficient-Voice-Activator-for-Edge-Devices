// feature_matrix.h
// The "2D matrix" your NN team's model reads: a rolling window of the most recent PCEN
// vectors, one row per hop. Implemented as a ring buffer (write index + wraparound) rather
// than shifting every element every 10ms -- that shift would be an O(context length x 40)
// memmove every hop, which is exactly the kind of avoidable cost the whole two-tier gate
// design is trying to eliminate elsewhere.
//
// NUM_CONTEXT_HOPS is a placeholder -- it's genuinely the NN team's call (how much time
// context their model architecture wants), not a DSP-sector decision. 100 hops = 1 second
// of context at a 10ms hop rate, which is a reasonable guess for a short custom wake word,
// but confirm against their actual model input shape.

#pragma once
#include "q_format.h"
#include "mel_filterbank.h"
#include <cstddef>

#define NUM_CONTEXT_HOPS 100 // PLACEHOLDER -- confirm against NN model's expected input shape

typedef struct {
    q15_16_t rows[NUM_CONTEXT_HOPS][NUM_MEL_FILTERS];
    int write_index;      // next row to overwrite
    bool full;            // has the buffer wrapped at least once
    q15_16_t last_vector[NUM_MEL_FILTERS]; // most recent row, cached for the tier-1-fail path
    bool has_last_vector;
} feature_matrix_t;

void feature_matrix_init(feature_matrix_t* matrix);

// Appends one new row (a PCEN vector), overwriting the oldest row once full. This is the
// one function called every single hop, whether or not either gate triggered -- what
// differs per hop is *which* vector gets passed in (see kws_frontend.cpp):
//   - tier 1 failed:  caller passes the cached last_vector again (duplicate, no new DSP work)
//   - tier 1 passed:  caller passes the freshly computed PCEN vector
void feature_matrix_append(feature_matrix_t* matrix, const q15_16_t* new_vector);

// Writes the context window out in correct time order (oldest row first) into
// out[NUM_CONTEXT_HOPS][NUM_MEL_FILTERS] -- the layout an NN inference call actually wants.
// Only needs to run right before an NN invocation (every ~50ms per your cadence), so an O(N)
// copy here is cheap relative to the inference itself.
void feature_matrix_get_ordered(const feature_matrix_t* matrix, q15_16_t out[NUM_CONTEXT_HOPS][NUM_MEL_FILTERS]);
