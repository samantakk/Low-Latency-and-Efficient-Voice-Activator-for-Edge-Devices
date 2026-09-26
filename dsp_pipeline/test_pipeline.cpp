// test_pipeline.cpp
// Desktop test harness (compiled with g++, not part of the firmware build).
// Three checks:
//   1. log2/exp2/pow_q15_16 accuracy vs. libm, across the range PCEN actually uses them in.
//   2. A synthetic tone through the full pipeline -- confirms the correct Mel channel lights up.
//   3. A float-reference re-implementation of the whole chain, run on the same input, to
//      quantify how much error the fixed-point approximations (mainly log2/exp2 in PCEN)
//      actually introduce end to end.

#include <cstdio>
#include <cmath>
#include <vector>
#include "q_format.h"
#include "fixed_math.h"
#include "framing.h"
#include "fft_fixed.h"
#include "mel_filterbank.h"
#include "pcen.h"
#include "pipeline.h"

// ---------------------------------------------------------------------------
// Test 1: log2/exp2/pow accuracy
// ---------------------------------------------------------------------------
static void test_fixed_math() {
    printf("=== Test 1: log2/exp2/pow accuracy ===\n");
    double max_log2_err = 0, max_exp2_err = 0, max_pow_err = 0;

    for (double x = 0.001; x < 100.0; x *= 1.37) {
        q15_16_t xq = float_to_q15_16((float)x);
        if (xq <= 0) continue;
        double ref = log2(x);
        double got = q15_16_to_float(log2_q15_16(xq));
        max_log2_err = fmax(max_log2_err, fabs(ref - got));
    }
    for (double x = -6.0; x < 6.0; x += 0.13) {
        q15_16_t xq = float_to_q15_16((float)x);
        double ref = pow(2.0, x);
        double got = q15_16_to_float(exp2_q15_16(xq));
        max_exp2_err = fmax(max_exp2_err, fabs(ref - got) / ref);
    }
    // pow with alpha=0.98 and r=0.5, the two exponents PCEN actually uses
    for (double x = 0.0001; x < 10.0; x *= 1.5) {
        q15_16_t xq = float_to_q15_16((float)x);
        if (xq <= 0) continue;
        for (double p : {0.98, 0.5}) {
            q15_16_t pq = float_to_q15_16((float)p);
            double ref = pow(x, p);
            double got = q15_16_to_float(pow_q15_16(xq, pq));
            double rel_err = fabs(ref - got) / (ref > 1e-6 ? ref : 1.0);
            max_pow_err = fmax(max_pow_err, rel_err);
        }
    }

    printf("  max |log2 error|          = %.6f (absolute, in bits)\n", max_log2_err);
    printf("  max exp2 relative error   = %.6f%%\n", max_exp2_err * 100.0);
    printf("  max pow(x^0.98 / x^0.5) relative error = %.6f%%\n", max_pow_err * 100.0);
    printf("\n");
}

// ---------------------------------------------------------------------------
// Test 2: synthetic tone through the full fixed-point pipeline
// ---------------------------------------------------------------------------
static void test_synthetic_tone() {
    printf("=== Test 2: %d Hz tone -> which Mel channel responds? ===\n", 1000);
    const double tone_freq = 1000.0;

    dsp_pipeline_t pipeline;
    dsp_pipeline_init(&pipeline);

    q15_16_t feature[NUM_MEL_FILTERS];
    q15_16_t frame[FRAME_SAMPLES];

    // Run several hops so the PCEN smoother (M_t) settles past its cold start.
    int num_hops = 20;
    for (int hop = 0; hop < num_hops; hop++) {
        for (int n = 0; n < FRAME_SAMPLES; n++) {
            double t = (hop * HOP_SAMPLES + n) / (double)SAMPLE_RATE_HZ;
            double sample = 0.5 * sin(2 * M_PI * tone_freq * t);
            frame[n] = float_to_q15_16((float)sample);
        }
        dsp_pipeline_process_frame(&pipeline, frame, feature);
    }

    // Report the top 3 responding channels on the final (settled) frame.
    int best[3] = {-1, -1, -1};
    for (int c = 0; c < NUM_MEL_FILTERS; c++) {
        float v = q15_16_to_float(feature[c]);
        for (int slot = 0; slot < 3; slot++) {
            if (best[slot] == -1 || q15_16_to_float(feature[best[slot]]) < v) {
                for (int s = 2; s > slot; s--) best[s] = best[s - 1];
                best[slot] = c;
                break;
            }
        }
    }
    for (int slot = 0; slot < 3; slot++) {
        int c = best[slot];
        printf("  #%d: Mel channel %d, PCEN value %.4f\n", slot + 1, c, q15_16_to_float(feature[c]));
    }

    // Sanity: 1000 Hz on a 0-8000 Hz, 40-filter Mel scale should land in the lower-middle
    // channels (Mel warping compresses high frequencies, so 1kHz isn't at channel 20/40).
    printf("  (expect this in the lower-third of the 40 channels -- Mel scale is nonlinear)\n\n");
}

// ---------------------------------------------------------------------------
// Test 3: float reference vs. fixed-point pipeline, same input, full chain
// ---------------------------------------------------------------------------
struct ComplexF { double re, im; };

static void float_fft(std::vector<ComplexF>& a) {
    int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        ComplexF wlen = {cos(ang), sin(ang)};
        for (int i = 0; i < n; i += len) {
            ComplexF w = {1, 0};
            for (int j = 0; j < len / 2; j++) {
                ComplexF u = a[i + j];
                ComplexF v = {a[i + j + len / 2].re * w.re - a[i + j + len / 2].im * w.im,
                              a[i + j + len / 2].re * w.im + a[i + j + len / 2].im * w.re};
                a[i + j] = {u.re + v.re, u.im + v.im};
                a[i + j + len / 2] = {u.re - v.re, u.im - v.im};
                ComplexF new_w = {w.re * wlen.re - w.im * wlen.im, w.re * wlen.im + w.im * wlen.re};
                w = new_w;
            }
        }
    }
}

// Deterministic pseudo-noise (no <random> dependency needed) -- simulates a mic's actual
// self-noise floor, which a real INMP441 has plenty of (~-87dBFS) and a mathematically pure
// synthetic sine does not. This matters: see the comment block at the end of this function.
static double cheap_noise(unsigned int* seed) {
    *seed = (*seed * 1103515245u + 12345u);
    return (((*seed >> 16) & 0x7fff) / 16384.0) - 1.0; // roughly uniform in [-1, 1)
}

static void test_float_vs_fixed() {
    printf("=== Test 3: float reference vs. fixed-point, full chain, same input ===\n");
    const double tone_freq = 700.0;
    const double noise_amplitude = 0.001; // ~-54dBFS relative to the 0.5-amplitude tone --
                                            // modest, well above Q15.16's quantization floor,
                                            // well below a real mic's actual self-noise.
    unsigned int noise_seed_fixed = 42, noise_seed_float = 42;

    // ---- Fixed-point run ----
    dsp_pipeline_t pipeline;
    dsp_pipeline_init(&pipeline);
    q15_16_t feature_fixed[NUM_MEL_FILTERS];
    q15_16_t frame[FRAME_SAMPLES];

    int num_hops = 15;
    for (int hop = 0; hop < num_hops; hop++) {
        for (int n = 0; n < FRAME_SAMPLES; n++) {
            double t = (hop * HOP_SAMPLES + n) / (double)SAMPLE_RATE_HZ;
            double sample = 0.5 * sin(2 * M_PI * tone_freq * t)
                          + noise_amplitude * cheap_noise(&noise_seed_fixed);
            frame[n] = float_to_q15_16((float)sample);
        }
        dsp_pipeline_process_frame(&pipeline, frame, feature_fixed);
    }

    // ---- Float reference run (same structure: window -> FFT -> periodogram -> Mel -> PCEN) ----
    // Re-derive Mel energies for every hop (needed for PCEN's running state), float-only.
    double M[NUM_MEL_FILTERS] = {0};
    double mel_energies_f[NUM_MEL_FILTERS];
    double feature_float[NUM_MEL_FILTERS];
    // NOTE: eps here is deliberately 1/65536, matching PCEN_EPS in pcen.cpp -- NOT the paper's
    // 1e-6. At Q15.16 resolution, 1e-6 rounds to zero, so the fixed pipeline substitutes the
    // smallest representable positive value. Using the paper's 1e-6 here would compare the
    // fixed pipeline against a reference it was never trying to match.
    const double s = 0.025, alpha = 0.98, delta = 2.0, r = 0.5, eps = 1.0 / 65536.0;

    for (int hop = 0; hop < num_hops; hop++) {
        std::vector<ComplexF> buf(FFT_SIZE, ComplexF{0, 0});
        for (int n = 0; n < FRAME_SAMPLES; n++) {
            double t = (hop * HOP_SAMPLES + n) / (double)SAMPLE_RATE_HZ;
            double sample = 0.5 * sin(2 * M_PI * tone_freq * t)
                          + noise_amplitude * cheap_noise(&noise_seed_float);
            double w = 0.54 - 0.46 * cos(2 * M_PI * n / (FRAME_SAMPLES - 1));
            buf[n].re = sample * w;
        }
        float_fft(buf);

        double periodogram[NUM_FFT_BINS];
        const double PERIODOGRAM_GAIN = 1024.0; // matches PERIODOGRAM_GAIN_SHIFT in mel_filterbank.cpp
        for (int k = 0; k < NUM_FFT_BINS; k++) {
            periodogram[k] = PERIODOGRAM_GAIN * (buf[k].re * buf[k].re + buf[k].im * buf[k].im) / FFT_SIZE;
        }
        for (int f = 0; f < NUM_MEL_FILTERS; f++) {
            const mel_filter_t* filt = &MEL_FILTERS[f];
            double sum = 0;
            for (int i = 0; i < filt->num_bins; i++) {
                int bin = filt->start_bin + i;
                if (bin >= NUM_FFT_BINS) break;
                sum += periodogram[bin] * q15_16_to_float(filt->weights[i]);
            }
            mel_energies_f[f] = sum;
        }
        const double energy_floor = 3277.0 / 65536.0; // matches PCEN_ENERGY_FLOOR in pcen.cpp
        for (int c = 0; c < NUM_MEL_FILTERS; c++) {
            double E = fmax(mel_energies_f[c], energy_floor);
            M[c] = (1 - s) * M[c] + s * E;
            double denom = pow(eps + M[c], alpha);
            double normalized = E / denom;
            double compressed = pow(normalized + delta, r);
            feature_float[c] = compressed - pow(delta, r);
        }
    }

    double max_abs_err = 0, sum_abs_err = 0;
    for (int c = 0; c < NUM_MEL_FILTERS; c++) {
        double fixed_val = q15_16_to_float(feature_fixed[c]);
        double err = fabs(fixed_val - feature_float[c]);
        max_abs_err = fmax(max_abs_err, err);
        sum_abs_err += err;
        printf("  ch %2d: M=%.6f  float=%.6f  fixed=%.6f  err=%.6f\n",
               c, M[c], feature_float[c], fixed_val, err);
    }
    printf("  max  |fixed - float| across 40 channels = %.6f\n", max_abs_err);
    printf("  mean |fixed - float| across 40 channels = %.6f\n", sum_abs_err / NUM_MEL_FILTERS);
    printf("  (channel values are typically O(1) after PCEN compression, so this is the\n");
    printf("   real-world error budget your NN team's classifier will actually see)\n\n");
}

int main() {
    test_fixed_math();
    test_synthetic_tone();
    test_float_vs_fixed();
    return 0;
}
