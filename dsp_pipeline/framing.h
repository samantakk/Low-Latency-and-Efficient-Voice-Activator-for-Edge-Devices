// framing.h
// Extracts overlapping 25ms frames (10ms hop) from a continuous 16kHz PCM stream and
// applies a Hamming window. The ring-buffer/streaming side of this (how frames arrive from
// I2S/DMA) is RTOS/Embedded C++ territory — this module just consumes whatever contiguous
// FRAME_SAMPLES-length window it's handed, already sample-rate-correct and level-normalized.

#pragma once
#include "q_format.h"
#include <cstddef>

#define SAMPLE_RATE_HZ   16000
#define FRAME_SAMPLES    400   // 25ms @ 16kHz
#define HOP_SAMPLES      160   // 10ms @ 16kHz

// Precomputed Q15.16 Hamming window, w[n] = 0.54 - 0.46*cos(2*pi*n/(FRAME_SAMPLES-1))
extern const q15_16_t HAMMING_WINDOW[FRAME_SAMPLES];

// Applies the window in place: frame[n] *= HAMMING_WINDOW[n]
void apply_hamming_window(q15_16_t* frame, size_t len);
