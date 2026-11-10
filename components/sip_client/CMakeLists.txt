idf_component_register(
    SRCS "sip_client.c"
    INCLUDE_DIRS "."
    REQUIRES config_manager wifi_manager rtp_audio
    PRIV_REQUIRES lwip mbedtls
)
