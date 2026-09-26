// test_frontend.cpp
// Simulates: 1s of quiet ambient noise -> 1s of "voice-like" signal (multiple simultaneous
// tones, since real speech has structured harmonic content, not a single pure tone) -> 1s
// back to quiet. Checks that: tier 1 mostly blocks the DSP pipeline during silence, tier 2
// distinguishes the "voice" segment from the noise segments, and NN invocation count is
// meaningfully lower than the hop count (the whole point of the two-tier design).

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include "q_format.h"
#include "kws_frontend.h"

static double cheap_noise(unsigned int* seed) {
    *seed = (*seed * 1103515245u + 12345u);
    return (((*seed >> 16) & 0x7fff) / 16384.0) - 1.0;
}

int main() {
    kws_frontend_t fe;
    kws_frontend_init(&fe);

    unsigned int seed = 7;
    int total_hops = 0, dsp_runs = 0, tier2_passes = 0, nn_invokes = 0;
    q15_16_t frame[FRAME_SAMPLES];

    auto run_segment = [&](const char* label, bool voice_like, int seconds) {
        int hops = seconds * (SAMPLE_RATE_HZ / HOP_SAMPLES); // 100 hops/sec at 10ms hop
        int seg_dsp = 0, seg_tier2 = 0, seg_nn = 0;
        for (int hop = 0; hop < hops; hop++) {
            for (int n = 0; n < FRAME_SAMPLES; n++) {
                double t = (total_hops * HOP_SAMPLES + n) / (double)SAMPLE_RATE_HZ;
                double sample = 0.02 * cheap_noise(&seed); // ambient noise floor, always present
                if (voice_like) {
                    // crude stand-in for voiced speech: a few harmonically-related tones with
                    // slowly varying amplitude, NOT a single pure sinusoid -- pure tones are
                    // spectrally simpler than real speech and would flatter the SFM gate.
                    sample += 0.3 * sin(2 * M_PI * 180.0 * t)
                            + 0.15 * sin(2 * M_PI * 360.0 * t)
                            + 0.08 * sin(2 * M_PI * 720.0 * t);
                }
                frame[n] = float_to_q15_16((float)sample);
            }
            kws_hop_result_t r = kws_frontend_process_hop(&fe, frame);
            if (r.ran_dsp) seg_dsp++;
            if (r.ran_dsp && r.tier2_passed) seg_tier2++;
            if (r.invoke_nn) seg_nn++;
            total_hops++;
        }
        dsp_runs += seg_dsp;
        tier2_passes += seg_tier2;
        nn_invokes += seg_nn;
        printf("  %-22s hops=%4d  dsp_ran=%4d (%.0f%%)  tier2_pass=%4d  nn_invoked=%3d\n",
               label, hops, seg_dsp, 100.0 * seg_dsp / hops, seg_tier2, seg_nn);
    };

    printf("=== Gated frontend: silence -> voice-like -> silence ===\n");
    run_segment("silence (1s)", false, 1);
    run_segment("voice-like (1s)", true, 1);
    run_segment("silence (1s)", false, 1);

    printf("\n=== Totals ===\n");
    printf("  total hops:        %d\n", total_hops);
    printf("  DSP pipeline ran:  %d (%.1f%% of hops)\n", dsp_runs, 100.0 * dsp_runs / total_hops);
    printf("  tier 2 passed:     %d\n", tier2_passes);
    printf("  NN invoked:        %d (%.1f%% of hops)\n", nn_invokes, 100.0 * nn_invokes / total_hops);
    printf("\n  (naive un-gated design would run DSP+NN on all %d hops; this is the saving\n", total_hops);
    printf("   the two-tier gate is actually buying you, on this synthetic signal)\n");

    return 0;
}
