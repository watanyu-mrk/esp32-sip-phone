/*
 * LVGL phone UI.
 *
 * ui_init() must be called from the LVGL task context or with
 * lvgl_port_lock() held. All widget access afterwards happens in the LVGL
 * task (timer/event callbacks), so no extra locking is required here.
 */
#pragma once

#include "esp_err.h"

esp_err_t ui_init(void);
