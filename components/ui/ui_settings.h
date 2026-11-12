/*
 * Settings screen, reachable from the dialpad's MENU button.
 *
 * Both functions must be called from the LVGL task (or with lvgl_port_lock()
 * held), like the rest of the UI.
 */
#pragma once

#include "lvgl.h"

/* Builds the settings screen once and remembers the dialpad screen to return to. */
void ui_settings_create(lv_obj_t *home_screen);

/* Switches the display to the settings screen. */
void ui_settings_open(void);
