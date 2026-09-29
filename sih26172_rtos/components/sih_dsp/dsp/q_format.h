// q_format.h
// Q15.16 fixed-point arithmetic: 1 sign bit + 15 integer bits + 16 fractional bits, in an int32_t.
// The binary point is a convention we track by hand, not a hardware feature — every function
// here exists to keep that bookkeeping correct.
//
// Range: approx -32768.0 to +32767.99998, resolution 1/65536 (~1.5e-5).
// This is plenty of headroom for normalized audio ([-1, 1)) and for Mel-filterbank energies,
// which is why it's used pipeline-wide instead of switching formats per stage.

#pragma once
#include <cstdint>
#include <cmath>

typedef int32_t q15_16_t;

#define Q15_16_SHIFT 16
#define Q15_16_ONE   ((q15_16_t)(1 << Q15_16_SHIFT))          // 1.0
#define Q15_16_HALF  ((q15_16_t)(1 << (Q15_16_SHIFT - 1)))    // 0.5

// ---- Conversion (host-side / offline table generation only — never call float math on-device) ----

static inline q15_16_t float_to_q15_16(float f) {
    float scaled = f * (float)(1 << Q15_16_SHIFT);
    scaled += (scaled >= 0.0f) ? 0.5f : -0.5f;   // round to nearest
    if (scaled > (float)INT32_MAX) return INT32_MAX;
    if (scaled < (float)INT32_MIN) return INT32_MIN;
    return (q15_16_t)scaled;
}

static inline float q15_16_to_float(q15_16_t q) {
    return (float)q / (float)(1 << Q15_16_SHIFT);
}

// ---- Saturating add/sub ----

static inline q15_16_t add_q15_16(q15_16_t a, q15_16_t b) {
    int64_t sum = (int64_t)a + (int64_t)b;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (q15_16_t)sum;
}

static inline q15_16_t sub_q15_16(q15_16_t a, q15_16_t b) {
    int64_t diff = (int64_t)a - (int64_t)b;
    if (diff > INT32_MAX) return INT32_MAX;
    if (diff < INT32_MIN) return INT32_MIN;
    return (q15_16_t)diff;
}

// ---- Multiply ----
// Two Q15.16 operands each carry one factor of 2^16. The raw product carries 2^32 —
// double what we want — so promote to int64_t first (avoid overflow), then correct with >>16.

static inline q15_16_t mul_q15_16(q15_16_t a, q15_16_t b) {
    int64_t product = (int64_t)a * (int64_t)b;
    product >>= Q15_16_SHIFT;
    if (product > INT32_MAX) return INT32_MAX;
    if (product < INT32_MIN) return INT32_MIN;
    return (q15_16_t)product;
}

// ---- Divide ----
// A plain integer divide of two Q15.16 values cancels both scale factors, leaving a
// zero-fractional-bit integer quotient — wrong for a Q15.16 result. Fix: pre-shift the
// numerator left by 16 (into int64_t, so it doesn't overflow) before dividing, injecting
// the extra 2^16 the result needs. Includes symmetric round-to-nearest and divide-by-zero
// saturation.

static inline q15_16_t div_q15_16(q15_16_t a, q15_16_t b) {
    if (b == 0) {
        return (a >= 0) ? INT32_MAX : INT32_MIN;
    }
    int64_t numerator = (int64_t)a << Q15_16_SHIFT;
    int64_t half_b = (int64_t)b / 2;
    // symmetric rounding: nudge numerator toward the quotient's rounded value before truncation
    if ((numerator >= 0) == (b >= 0)) {
        numerator += half_b;
    } else {
        numerator -= half_b;
    }
    int64_t result = numerator / b;
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (q15_16_t)result;
}
