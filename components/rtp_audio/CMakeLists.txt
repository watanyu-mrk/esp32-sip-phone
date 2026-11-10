idf_component_register(
    SRCS "rtp_client.c" "rtp_jitter.c"
    INCLUDE_DIRS "."
    REQUIRES lwip audio_driver
)
