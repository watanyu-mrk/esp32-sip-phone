if(CONFIG_SIP_PHONE_AUDIO_INMP441)
    set(audio_srcs "audio_i2s_input.c" "audio_i2s_output.c" "audio_process.c")
else()
    set(audio_srcs "audio_input.c" "audio_output.c" "audio_process.c")
endif()

idf_component_register(
    SRCS ${audio_srcs}
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer config_manager
)
