idf_component_register(
    SRCS "main.c" "app_state.c"
    INCLUDE_DIRS "."
    REQUIRES
        config_manager
        wifi_manager
        audio_driver
        sip_client
        rtp_audio
        phone_control
        web_server
        esp_timer
)
