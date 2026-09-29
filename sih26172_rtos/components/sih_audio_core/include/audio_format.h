/*
 * audio_format.h - sample format definitions and conversions (pure C, no ESP-IDF dependency)
 *
 * INTERFACE #1 with the DSP/NN team (implementation.md "Interfaces" item 1):
 *   The ring buffer holds normalized Q15.16 samples, never raw I2S words.
 *   Full scale of the microphone maps to [-1.0, +1.0) which is [-65536, +65535] in Q15.16.
 *   dsp_task never has to know about I2S bit packing.
 *
 * This header is also compiled on the host by test/host/run_tests.sh.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signed fixed point: 1 sign bit, 15 integer bits, 16 fractional bits. 1.0 == 65536.
 *
 * These three definitions are deliberately IDENTICAL, token for token, to the DSP team's
 * q_format.h (Q15_16_SHIFT / Q15_16_ONE / typedef). Identical redefinitions are legal, so a
 * translation unit that includes both headers compiles (it used to fail: Q15_16_ONE and
 * q15_16_to_float() were defined differently here). Do not "tidy" them. */
typedef int32_t q15_16_t;

#define Q15_16_SHIFT 16
#define Q15_16_ONE   ((q15_16_t)(1 << Q15_16_SHIFT))
#define Q15_16_FRAC_BITS Q15_16_SHIFT

/*
 * Convert one 32-bit word received from I2S into a normalized Q15.16 sample.
 *
 * Bit layout on the wire (e.g. SPH0645: 18-bit sample, 24-bit slot, 32-bit I2S word):
 *
 *   bit 31                    14 13       8 7        0
 *   [ 18-bit two's complement   ][ 6 x 0   ][ 8 x pad ]
 *   |<-------- 24-bit slot ------------------>|
 *   |<----------------- 32-bit word ------------------->|
 *
 * The sample is MSB-aligned, so an arithmetic right shift by (32 - sample_bits)
 * both sign-extends it and throws away the padding bits, whatever they contain.
 *
 * Normalization: an N-bit sample has full scale 2^(N-1). Q15.16 full scale (1.0) is 2^16.
 *   q = s * 2^16 / 2^(N-1) = s >> (N - 17)          for N >= 17
 *   q = s * 2^(17 - N)                             for N <  17
 * For N = 18 this is s >> 1: Q15.16 has 16 fractional bits, so the 18th bit of the
 * sample (about -102 dBFS, far below any MEMS mic noise floor) is dropped. The shift
 * floors toward minus infinity (half-LSB bias, also negligible).
 *
 * GCC defines >> on negative signed integers as an arithmetic shift, which is what
 * we rely on here (both xtensa-esp-elf-gcc and host gcc/clang).
 *
 * sample_bits must be in [2, 32]; the project restricts it to [16, 32] via Kconfig.
 */
static inline q15_16_t i2s_word_to_q15_16(uint32_t word, unsigned sample_bits)
{
    const int32_t s = ((int32_t)word) >> (32u - sample_bits);
    if (sample_bits >= 17u) {
        return (q15_16_t)(s >> (sample_bits - 17u));
    }
    return (q15_16_t)(s * ((int32_t)1 << (17u - sample_bits)));
}

/*
 * Q15.16 normalized sample -> signed 16-bit PCM (what the ASR server receives).
 * 1.0 (65536) would be 32768, which does not fit, so the result saturates.
 */
static inline int16_t q15_16_to_pcm16(q15_16_t q)
{
    int32_t v = q >> 1; /* 2^16 full scale -> 2^15 full scale */
    if (v > INT16_MAX) {
        v = INT16_MAX;
    } else if (v < INT16_MIN) {
        v = INT16_MIN;
    }
    return (int16_t)v;
}

/* Named sih_* so it cannot collide with q_format.h's C++ q15_16_to_float(). */
static inline float sih_q15_16_to_float(q15_16_t q)
{
    return (float)q * (1.0f / (float)Q15_16_ONE);
}

#ifdef __cplusplus
}
#endif
