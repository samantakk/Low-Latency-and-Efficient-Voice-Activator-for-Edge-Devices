// mel_filterbank.h
// Converts the 257-bin power spectrum (periodogram of a 512-pt real FFT) into 40 Mel-band
// energies. Filters are stored sparse (start_bin, num_bins, weight pointer) rather than as
// full 257-wide zero-padded vectors — each triangle only touches a handful of bins, so this
// is a meaningful RAM saving on top of being a meaningful compute saving.
//
// The dot product per filter is: sum(periodogram[start_bin + i] * weights[i]) for i in [0, num_bins).

#pragma once
#include "q_format.h"
#include "fft_fixed.h"
#include <cstdint>
#include <cstddef>

#define NUM_MEL_FILTERS 40
#define NUM_FFT_BINS (FFT_SIZE / 2 + 1) // 257 unique bins for a real-input 512-pt FFT

typedef struct {
    uint16_t start_bin;
    uint16_t num_bins;
    const q15_16_t* weights;
} mel_filter_t;

extern const q15_16_t MEL_FILTER_WEIGHTS[];
extern const mel_filter_t MEL_FILTERS[NUM_MEL_FILTERS];

// periodogram: NUM_FFT_BINS Q15.16 values (|FFT|^2/N, computed by the caller from fft output).
// mel_energies_out: NUM_MEL_FILTERS Q15.16 values.
void apply_mel_filterbank(const q15_16_t* periodogram, q15_16_t* mel_energies_out);

// Computes the periodogram (|FFT(frame)|^2 / N) from FFT output, writing NUM_FFT_BINS values.
void compute_periodogram(const complex_q15_16_t* fft_output, q15_16_t* periodogram_out);
