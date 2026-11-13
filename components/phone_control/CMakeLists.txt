idf_component_register(
    SRCS "phone_control.c" "button.c"
    INCLUDE_DIRS "."
    REQUIRES sip_client rtp_audio config_manager ringtone driver esp_timer
)
