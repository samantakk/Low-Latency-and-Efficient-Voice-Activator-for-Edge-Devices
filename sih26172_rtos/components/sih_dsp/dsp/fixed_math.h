// fixed_math.h
// Transcendental-ish helpers built on q_format.h. Nothing here calls libm at runtime —
// float only ever appears at compile-time when generating the SQRT2_POWERS table below,
// which is baked into constants exactly like a twiddle-factor table.
//
// Needed because PCEN's compression step raises energies to fractional powers:
//   (E / (eps+M)^alpha + delta)^r      alpha ~ 0.98, r = 0.5
// Fixed-point has no native x^alpha. The standard trick: x^alpha = 2^(alpha * log2(x)),
// so we need fixed-point log2 and exp2.

#pragma once
#include "q_format.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// sqrt_q15_16 — exact integer square root, no approximation needed for r=0.5.
// ---------------------------------------------------------------------------
// v = x_int / 65536. We want y_int = sqrt(v) * 65536 = sqrt(x_int * 65536) = sqrt(x_int << 16).
// Computed with the classic bitwise integer sqrt (digit-by-digit), so it's exact (floor),
// deterministic, and needs no FPU — good for an MCU target.

static inline uint64_t isqrt64(uint64_t n) {
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62; // highest even power of 2 <= range of n
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        uint64_t candidate = result + bit;
        if (n >= candidate) {
            n -= candidate;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static inline q15_16_t sqrt_q15_16(q15_16_t x) {
    if (x <= 0) return 0;
    return (q15_16_t)isqrt64((uint64_t)x << Q15_16_SHIFT);
}

// ---------------------------------------------------------------------------
// log2_q15_16 — bit-by-bit fractional log2 via repeated squaring.
// ---------------------------------------------------------------------------
// For y in [1,2): log2(y) = 0.b1 b2 b3 ... in binary. Squaring y gives 2^(b1.b2b3...):
// if y*y >= 2, that means b1 = 1 and we bring y back into [1,2) by halving; else b1 = 0.
// Repeat for b2, b3, ... — each iteration resolves one more fractional bit, same idea
// as the bit-reversal/twiddle bookkeeping you're already doing for the FFT.

#define LOG2_FRAC_ITERS 16  // matches Q15.16's 16 fractional bits

static inline q15_16_t log2_q15_16(q15_16_t x) {
    if (x <= 0) return INT32_MIN; // log2 undefined; caller must guard (PCEN adds eps first)

    // Find integer part: n = floor(log2(x_real)) where x_real = x / 65536.
    // bit_pos = index of MSB of x (0-indexed), so x_real is in [2^(bit_pos-16), 2^(bit_pos-15)).
    int bit_pos = 31 - __builtin_clz((uint32_t)x);
    int n = bit_pos - Q15_16_SHIFT;

    // Normalize x into y in [1.0, 2.0) i.e. [65536, 131072) in Q15.16.
    q15_16_t y;
    int shift = bit_pos - Q15_16_SHIFT; // how far MSB sits above bit 16
    if (shift >= 0) y = x >> shift;
    else            y = x << (-shift);

    q15_16_t frac = 0;
    q15_16_t weight = Q15_16_HALF; // 2^-1
    for (int i = 0; i < LOG2_FRAC_ITERS; i++) {
        y = mul_q15_16(y, y);           // y now in [1, 4)
        if (y >= (Q15_16_ONE << 1)) {   // >= 2.0 -> this bit is 1
            frac = add_q15_16(frac, weight);
            y >>= 1;                    // divide by 2 to bring back into [1, 2)
        }
        weight >>= 1;
    }

    return add_q15_16((q15_16_t)(n << Q15_16_SHIFT), frac);
}

// ---------------------------------------------------------------------------
// exp2_q15_16 — inverse of the above: reconstruct 2^f from f's binary digits.
// ---------------------------------------------------------------------------
// f = 0.b1 b2 b3... in [0,1). Then 2^f = product over i of (2^(2^-i)) for every bit i that's 1 —
// i.e. 2^0.101 = sqrt(2) * 2^0.125^0 * (2^(2^-3)) etc. Precompute the roots-of-two table once
// (compile-time constants, same status as a twiddle table) and multiply the ones whose bit is set.

#define EXP2_FRAC_ITERS 16

// SQRT2_POWERS[i] = 2^(2^-(i+1)) in Q15.16, i.e. index 0 = sqrt(2), index 1 = 2^0.25, ...
static const q15_16_t SQRT2_POWERS[EXP2_FRAC_ITERS] = {
    92682,  // 2^(1/2)     = 1.41421356
    77936,  // 2^(1/4)     = 1.18920712
    71468,  // 2^(1/8)     = 1.09050773
    68438,  // 2^(1/16)    = 1.04427378
    66971,  // 2^(1/32)    = 1.02189715
    66250,  // 2^(1/64)    = 1.01088929
    65892,  // 2^(1/128)   = 1.00542990
    65714,  // 2^(1/256)   = 1.00271128
    65625,  // 2^(1/512)   = 1.00135472
    65580,  // 2^(1/1024)  = 1.00067713
    65558,  // 2^(1/2048)  = 1.00033851
    65547,  // 2^(1/4096)  = 1.00016924
    65542,  // 2^(1/8192)  = 1.00008462
    65539,  // 2^(1/16384) = 1.00004231
    65537,  // 2^(1/32768) = 1.00002115
    65537   // 2^(1/65536) = 1.00001058
};

// 2^f for f in [0, Q15_16_ONE) (i.e. fractional part only)
static inline q15_16_t exp2_frac_q15_16(q15_16_t f) {
    q15_16_t y = Q15_16_ONE;
    q15_16_t weight = Q15_16_HALF;
    for (int i = 0; i < EXP2_FRAC_ITERS; i++) {
        if (f >= weight) {
            y = mul_q15_16(y, SQRT2_POWERS[i]);
            f -= weight;
        }
        weight >>= 1;
    }
    return y;
}

// Full 2^x for signed Q15.16 x. Only safe for modest |n| (PCEN exponents keep results
// well within Q15.16 range); no overflow guard beyond mul_q15_16's own saturation.
static inline q15_16_t exp2_q15_16(q15_16_t x) {
    // floor division/mod that behaves for negative x
    int32_t n = x >> Q15_16_SHIFT;              // arithmetic shift = floor for two's complement
    q15_16_t frac = x - (n << Q15_16_SHIFT);    // always in [0, Q15_16_ONE)

    q15_16_t result = exp2_frac_q15_16(frac);
    if (n >= 0) {
        if (n >= 31) return (result > 0) ? INT32_MAX : 0; // overflow guard
        int64_t shifted = (int64_t)result << n;
        return (shifted > INT32_MAX) ? INT32_MAX : (q15_16_t)shifted;
    } else {
        if (-n >= 31) return 0;
        return (q15_16_t)(result >> (-n));
    }
}

// ---------------------------------------------------------------------------
// pow_q15_16 — x^p for x > 0, via x^p = 2^(p * log2(x)).
// ---------------------------------------------------------------------------

static inline q15_16_t pow_q15_16(q15_16_t x, q15_16_t p) {
    if (x <= 0) return 0;
    return exp2_q15_16(mul_q15_16(p, log2_q15_16(x)));
}
