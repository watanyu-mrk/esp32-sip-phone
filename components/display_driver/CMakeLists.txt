idf_component_register(
    SRCS "display_driver.c" "nv3041_panel.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer
)
