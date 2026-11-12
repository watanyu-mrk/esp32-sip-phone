idf_component_register(
    SRCS "lvgl_port.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_timer display_driver touch_driver
    PRIV_REQUIRES lvgl
)
