# C++ DSP frontend (dsp/ = the DSP team's sources, copied UNMODIFIED from dsp_pipeline.zip)
# plus kws_fe.cpp, the extern "C" bridge the (pure C) RTOS code calls.
#
# Only include/ is exported. The DSP headers are deliberately NOT on any other
# component's include path: dsp/pipeline.h would otherwise be ambiguous with main/pipeline.h,
# and dsp/q_format.h would clash with sih_audio_core/audio_format.h.
idf_component_register(SRCS "kws_fe.cpp"
                            "dsp/feature_matrix.cpp"
                            "dsp/fft_fixed.cpp"
                            "dsp/framing.cpp"
                            "dsp/kws_frontend.cpp"
                            "dsp/mel_filterbank.cpp"
                            "dsp/pcen.cpp"
                            "dsp/pipeline.cpp"
                            "dsp/vad.cpp"
                       INCLUDE_DIRS "include")
