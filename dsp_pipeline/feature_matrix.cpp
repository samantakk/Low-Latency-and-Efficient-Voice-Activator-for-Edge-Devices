// feature_matrix.cpp
#include "feature_matrix.h"
#include <cstring>

void feature_matrix_init(feature_matrix_t* matrix) {
    matrix->write_index = 0;
    matrix->full = false;
    matrix->has_last_vector = false;
    for (int i = 0; i < NUM_CONTEXT_HOPS; i++)
        for (int c = 0; c < NUM_MEL_FILTERS; c++)
            matrix->rows[i][c] = 0;
}

void feature_matrix_append(feature_matrix_t* matrix, const q15_16_t* new_vector) {
    memcpy(matrix->rows[matrix->write_index], new_vector, sizeof(q15_16_t) * NUM_MEL_FILTERS);
    memcpy(matrix->last_vector, new_vector, sizeof(q15_16_t) * NUM_MEL_FILTERS);
    matrix->has_last_vector = true;

    matrix->write_index++;
    if (matrix->write_index >= NUM_CONTEXT_HOPS) {
        matrix->write_index = 0;
        matrix->full = true;
    }
}

void feature_matrix_get_ordered(const feature_matrix_t* matrix, q15_16_t out[NUM_CONTEXT_HOPS][NUM_MEL_FILTERS]) {
    if (!matrix->full) {
        // Not wrapped yet: rows [0, write_index) are the only real data, oldest-first already.
        // Rows beyond that are still the zero-initialized padding from feature_matrix_init --
        // whether that's the right padding convention (vs. e.g. repeating row 0) is worth
        // confirming with the NN team once real training data shows what the model expects
        // to see during the first second after boot.
        memcpy(out, matrix->rows, sizeof(matrix->rows));
        return;
    }
    // Wrapped: oldest row is at write_index, newest is right before it.
    int dest = 0;
    for (int i = matrix->write_index; i < NUM_CONTEXT_HOPS; i++, dest++)
        memcpy(out[dest], matrix->rows[i], sizeof(q15_16_t) * NUM_MEL_FILTERS);
    for (int i = 0; i < matrix->write_index; i++, dest++)
        memcpy(out[dest], matrix->rows[i], sizeof(q15_16_t) * NUM_MEL_FILTERS);
}
