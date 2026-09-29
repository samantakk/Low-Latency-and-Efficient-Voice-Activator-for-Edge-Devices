// mel_filterbank.cpp
#include "mel_filterbank.h"

// Auto-generated Mel filterbank tables: 40 filters, FFT size 512, 0-8000 Hz, 16000Hz sample rate
const q15_16_t MEL_FILTER_WEIGHTS[534] = {
    0, 65536, 32768, 0, 0, 32768, 65536, 32768,
    0, 0, 32768, 65536, 0, 0, 65536, 32768,
    0, 0, 32768, 65536, 32768, 0, 0, 32768,
    65536, 32768, 0, 0, 32768, 65536, 32768, 0,
    0, 32768, 65536, 43691, 21845, 0, 0, 21845,
    43691, 65536, 32768, 0, 0, 32768, 65536, 43691,
    21845, 0, 0, 21845, 43691, 65536, 32768, 0,
    0, 32768, 65536, 43691, 21845, 0, 0, 21845,
    43691, 65536, 49152, 32768, 16384, 0, 0, 16384,
    32768, 49152, 65536, 43691, 21845, 0, 0, 21845,
    43691, 65536, 43691, 21845, 0, 0, 21845, 43691,
    65536, 49152, 32768, 16384, 0, 0, 16384, 32768,
    49152, 65536, 49152, 32768, 16384, 0, 0, 16384,
    32768, 49152, 65536, 52429, 39322, 26214, 13107, 0,
    0, 13107, 26214, 39322, 52429, 65536, 49152, 32768,
    16384, 0, 0, 16384, 32768, 49152, 65536, 52429,
    39322, 26214, 13107, 0, 0, 13107, 26214, 39322,
    52429, 65536, 52429, 39322, 26214, 13107, 0, 0,
    13107, 26214, 39322, 52429, 65536, 54613, 43691, 32768,
    21845, 10923, 0, 0, 10923, 21845, 32768, 43691,
    54613, 65536, 54613, 43691, 32768, 21845, 10923, 0,
    0, 10923, 21845, 32768, 43691, 54613, 65536, 54613,
    43691, 32768, 21845, 10923, 0, 0, 10923, 21845,
    32768, 43691, 54613, 65536, 54613, 43691, 32768, 21845,
    10923, 0, 0, 10923, 21845, 32768, 43691, 54613,
    65536, 56174, 46811, 37449, 28087, 18725, 9362, 0,
    0, 9362, 18725, 28087, 37449, 46811, 56174, 65536,
    57344, 49152, 40960, 32768, 24576, 16384, 8192, 0,
    0, 8192, 16384, 24576, 32768, 40960, 49152, 57344,
    65536, 57344, 49152, 40960, 32768, 24576, 16384, 8192,
    0, 0, 8192, 16384, 24576, 32768, 40960, 49152,
    57344, 65536, 57344, 49152, 40960, 32768, 24576, 16384,
    8192, 0, 0, 8192, 16384, 24576, 32768, 40960,
    49152, 57344, 65536, 58254, 50972, 43691, 36409, 29127,
    21845, 14564, 7282, 0, 0, 7282, 14564, 21845,
    29127, 36409, 43691, 50972, 58254, 65536, 58982, 52429,
    45875, 39322, 32768, 26214, 19661, 13107, 6554, 0,
    0, 6554, 13107, 19661, 26214, 32768, 39322, 45875,
    52429, 58982, 65536, 58982, 52429, 45875, 39322, 32768,
    26214, 19661, 13107, 6554, 0, 0, 6554, 13107,
    19661, 26214, 32768, 39322, 45875, 52429, 58982, 65536,
    59578, 53620, 47663, 41705, 35747, 29789, 23831, 17873,
    11916, 5958, 0, 0, 5958, 11916, 17873, 23831,
    29789, 35747, 41705, 47663, 53620, 59578, 65536, 59578,
    53620, 47663, 41705, 35747, 29789, 23831, 17873, 11916,
    5958, 0, 0, 5958, 11916, 17873, 23831, 29789,
    35747, 41705, 47663, 53620, 59578, 65536, 60075, 54613,
    49152, 43691, 38229, 32768, 27307, 21845, 16384, 10923,
    5461, 0, 0, 5461, 10923, 16384, 21845, 27307,
    32768, 38229, 43691, 49152, 54613, 60075, 65536, 60495,
    55454, 50412, 45371, 40330, 35289, 30247, 25206, 20165,
    15124, 10082, 5041, 0, 0, 5041, 10082, 15124,
    20165, 25206, 30247, 35289, 40330, 45371, 50412, 55454,
    60495, 65536, 60855, 56174, 51493, 46811, 42130, 37449,
    32768, 28087, 23406, 18725, 14043, 9362, 4681, 0,
    0, 4681, 9362, 14043, 18725, 23406, 28087, 32768,
    37449, 42130, 46811, 51493, 56174, 60855, 65536, 61167,
    56798, 52429, 48060, 43691, 39322, 34953, 30583, 26214,
    21845, 17476, 13107, 8738, 4369, 0, 0, 4369,
    8738, 13107, 17476, 21845, 26214, 30583, 34953, 39322,
    43691, 48060, 52429, 56798, 61167, 65536, 61167, 56798,
    52429, 48060, 43691, 39322, 34953, 30583, 26214, 21845,
    17476, 13107, 8738, 4369, 0, 0, 4369, 8738,
    13107, 17476, 21845, 26214, 30583, 34953, 39322, 43691,
    48060, 52429, 56798, 61167, 65536, 61681, 57826, 53971,
    50116, 46261, 42406, 38551, 34696, 30840, 26985, 23130,
    19275, 15420, 11565, 7710, 3855, 0,
};

const mel_filter_t MEL_FILTERS[NUM_MEL_FILTERS] = {
    { 0, 4, &MEL_FILTER_WEIGHTS[0] },
    { 1, 5, &MEL_FILTER_WEIGHTS[4] },
    { 3, 4, &MEL_FILTER_WEIGHTS[9] },
    { 5, 4, &MEL_FILTER_WEIGHTS[13] },
    { 6, 5, &MEL_FILTER_WEIGHTS[17] },
    { 8, 5, &MEL_FILTER_WEIGHTS[22] },
    { 10, 5, &MEL_FILTER_WEIGHTS[27] },
    { 12, 6, &MEL_FILTER_WEIGHTS[32] },
    { 14, 6, &MEL_FILTER_WEIGHTS[38] },
    { 17, 6, &MEL_FILTER_WEIGHTS[44] },
    { 19, 6, &MEL_FILTER_WEIGHTS[50] },
    { 22, 6, &MEL_FILTER_WEIGHTS[56] },
    { 24, 8, &MEL_FILTER_WEIGHTS[62] },
    { 27, 8, &MEL_FILTER_WEIGHTS[70] },
    { 31, 7, &MEL_FILTER_WEIGHTS[78] },
    { 34, 8, &MEL_FILTER_WEIGHTS[85] },
    { 37, 9, &MEL_FILTER_WEIGHTS[93] },
    { 41, 10, &MEL_FILTER_WEIGHTS[102] },
    { 45, 10, &MEL_FILTER_WEIGHTS[112] },
    { 50, 10, &MEL_FILTER_WEIGHTS[122] },
    { 54, 11, &MEL_FILTER_WEIGHTS[132] },
    { 59, 12, &MEL_FILTER_WEIGHTS[143] },
    { 64, 13, &MEL_FILTER_WEIGHTS[155] },
    { 70, 13, &MEL_FILTER_WEIGHTS[168] },
    { 76, 13, &MEL_FILTER_WEIGHTS[181] },
    { 82, 14, &MEL_FILTER_WEIGHTS[194] },
    { 88, 16, &MEL_FILTER_WEIGHTS[208] },
    { 95, 17, &MEL_FILTER_WEIGHTS[224] },
    { 103, 17, &MEL_FILTER_WEIGHTS[241] },
    { 111, 18, &MEL_FILTER_WEIGHTS[258] },
    { 119, 20, &MEL_FILTER_WEIGHTS[276] },
    { 128, 21, &MEL_FILTER_WEIGHTS[296] },
    { 138, 22, &MEL_FILTER_WEIGHTS[317] },
    { 148, 23, &MEL_FILTER_WEIGHTS[339] },
    { 159, 24, &MEL_FILTER_WEIGHTS[362] },
    { 170, 26, &MEL_FILTER_WEIGHTS[386] },
    { 182, 28, &MEL_FILTER_WEIGHTS[412] },
    { 195, 30, &MEL_FILTER_WEIGHTS[440] },
    { 209, 31, &MEL_FILTER_WEIGHTS[470] },
    { 224, 33, &MEL_FILTER_WEIGHTS[501] },
};

void apply_mel_filterbank(const q15_16_t* periodogram, q15_16_t* mel_energies_out) {
    for (int f = 0; f < NUM_MEL_FILTERS; f++) {
        const mel_filter_t* filt = &MEL_FILTERS[f];
        q15_16_t sum = 0;
        for (int i = 0; i < filt->num_bins; i++) {
            int bin = filt->start_bin + i;
            if (bin >= NUM_FFT_BINS) break; // guard against table edge rounding
            sum = add_q15_16(sum, mul_q15_16(periodogram[bin], filt->weights[i]));
        }
        mel_energies_out[f] = sum;
    }
}

void compute_periodogram(const complex_q15_16_t* fft_output, q15_16_t* periodogram_out) {
    // IMPORTANT: fft_fixed_512 right-shifts by 1 after each of its 9 stages to prevent
    // overflow, so its output is the true FFT divided by FFT_SIZE (512). The periodogram
    // formula |X_true|^2/N therefore needs a *512 correction applied to the magnitude-squared
    // of what we actually have (X_true/512).
    //
    // The wrong way to apply that correction: mul_q15_16(re,re) first (which internally does
    // >>16, truncating to Q15.16), then left-shift the *result* by 9 to multiply by 512.
    // That truncates 16 fractional bits and only gets 9 of them back -- for a quiet spectral
    // bin (windowing sidelobe, background noise), re/im are already small, and squaring a
    // small Q15.16 value routinely truncates to exactly 0 *before* the correction ever runs.
    // That bin then reads as pure silence to PCEN downstream, which is exactly the kind of
    // false negative that matters for the false-activation-rate risk on real hardware.
    //
    // Fix: do the squaring in full 64-bit precision first (no truncation), and apply the
    // *entire* correction (512 / 65536 = >>7) in one shift at the end. Algebraically:
    //   periodogram_raw = (re_raw^2 + im_raw^2) * 512 / 65536 = (re_raw^2 + im_raw^2) >> 7
    // This keeps 9 more bits of precision than the two-step version.
    //
    // PERIODOGRAM_GAIN: separately from the truncation fix above, this is a genuine precision
    // finding, not a truncation bug: even with the fix above, PCEN's ratio-based math (E /
    // (eps+M)^alpha) is extremely sensitive to the LSB-level noise floor of whatever's stored,
    // because normalizing quiet channels *up* to loud-channel scale is the entire point of
    // PCEN -- a few raw units of quantization noise in a quiet channel becomes a huge relative
    // error right where the math amplifies it most. Peak periodogram values for normalized
    // ([-1,1)) audio sit around single digits, nowhere near Q15.16's ~32767 ceiling, so there's
    // free headroom sitting unused. Pre-multiplying by 1024 spends that headroom on resolution
    // exactly where PCEN needs it, at zero extra cost -- the combined scaling below works out
    // to a left-shift (more precision kept, not less) rather than an additional right-shift.
    // (This does shift PCEN's absolute output scale by a small, constant, deployment-wide
    // factor -- not something a classifier trained on this pipeline's own output ever sees.)
    #define PERIODOGRAM_GAIN_SHIFT 10  // x1024
    #define PERIODOGRAM_COMBINED_SHIFT (PERIODOGRAM_GAIN_SHIFT + FFT_LOG2_SIZE - Q15_16_SHIFT) // = 3

    for (int k = 0; k < NUM_FFT_BINS; k++) {
        int64_t re = fft_output[k].re;
        int64_t im = fft_output[k].im;
        int64_t sum_sq = re * re + im * im;   // full precision, no intermediate truncation
        int64_t corrected = (PERIODOGRAM_COMBINED_SHIFT >= 0)
            ? (sum_sq << PERIODOGRAM_COMBINED_SHIFT)
            : (sum_sq >> (-PERIODOGRAM_COMBINED_SHIFT));
        if (corrected > INT32_MAX) corrected = INT32_MAX;
        periodogram_out[k] = (q15_16_t)corrected;
    }
}
