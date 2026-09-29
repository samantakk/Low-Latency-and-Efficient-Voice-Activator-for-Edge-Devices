# NN inference: TFLite Micro + the NN team's model (kws_model_data.h, unmodified) and the
# glue from their esp32_kws_firmware.cpp (model init, input quantization, invoke, bouncer).
# Their tasks / ISR / app_main are NOT built: the RTOS project owns those (see reference/).
idf_component_register(SRCS "kws_nn.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES sih_dsp
                       PRIV_REQUIRES espressif__esp-tflite-micro log)
