idf_component_register(
    SRCS "ui.c" "ui_settings.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl lvgl_port config_manager phone_control sip_client wifi_manager
             display_driver audio_driver ringtone esp_system
)
