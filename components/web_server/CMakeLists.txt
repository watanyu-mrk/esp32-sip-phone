idf_component_register(
    SRCS "web_server.c" "web_api.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server config_manager wifi_manager audio_driver sip_client phone_control ringtone json mbedtls
    EMBED_TXTFILES "../../web/index.html" "../../web/style.css" "../../web/app.js"
)
