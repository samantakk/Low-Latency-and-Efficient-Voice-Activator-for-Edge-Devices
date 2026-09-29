// fft_fixed.h
// Radix-2 Decimation-In-Time FFT, Q15.16 fixed-point, N=512.
// Convention: bit-reversed input -> natural-order output (matches your professor's diagrams).
//
// Overflow handling: each butterfly stage can grow the signal magnitude by up to 2x.
// Over log2(512)=9 stages that's a possible 512x growth. Standard fix (used here): after
// every stage, right-shift all values by 1 bit ("stage scaling"). This trades a small,
// known amount of resolution (1 bit per stage = 9 bits total, acceptable inside Q15.16's
// 16 fractional bits) for a hard overflow guarantee, instead of saturating unpredictably.

#pragma once
#include "q_format.h"
#include <cstdint>
#include <cstddef>

#define FFT_SIZE 512
#define FFT_LOG2_SIZE 9

typedef struct {
    q15_16_t re;
    q15_16_t im;
} complex_q15_16_t;

// Twiddle table: W_N^k = exp(-j*2*pi*k/N) for k = 0 .. N/2-1 (only need half by symmetry).
extern const complex_q15_16_t FFT_TWIDDLES[FFT_SIZE / 2];

// In-place FFT. `data` must already be in bit-reversed order (see fft_bit_reverse_reorder).
void fft_fixed_512(complex_q15_16_t* data);

// Reorders `data` (length FFT_SIZE) into bit-reversed index order, in place.
void fft_bit_reverse_reorder(complex_q15_16_t* data);

// Convenience: zero-pad a real Q15.16 frame of `frame_len` samples into a full-size
// complex buffer ready for fft_bit_reverse_reorder + fft_fixed_512.
void fft_load_real_frame(const q15_16_t* frame, size_t frame_len, complex_q15_16_t* out);
