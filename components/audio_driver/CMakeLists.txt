idf_component_register(
    SRCS "audio_input.c" "audio_output.c" "audio_process.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer config_manager
)
