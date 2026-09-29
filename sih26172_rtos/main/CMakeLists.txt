set(srcs
    "main.c"
    "pipeline.c"
    "audio_ingest.c"
    "dsp_task.c"
    "nn_task.c"
    "net_stream.c"
    "wifi_link.c"
    "sys_stats.c")

# Pinned server certificate for wss:// to a self-signed laptop server
# (menuconfig: Server certificate check -> Pinned certificate). Create it with
# tools/make_cert.sh. Not embedded otherwise, so the file is optional.
set(embed_txt "")
if(CONFIG_SIH_WS_TLS_EMBEDDED_CERT)
    list(APPEND embed_txt "certs/server_cert.pem")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES sih_audio_core
                                sih_dsp
                                sih_nn
                                esp_driver_i2s
                                esp_driver_gpio
                                esp_timer
                                esp_pm
                                esp_wifi
                                esp_netif
                                esp_event
                                nvs_flash
                                mbedtls
                                esp_websocket_client
                       EMBED_TXTFILES ${embed_txt})
