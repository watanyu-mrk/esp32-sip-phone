/*
 * LVGL 9 port: display, input device, tick, render task and locking.
 *
 * Threading contract:
 *   - LVGL APIs may only be called from the LVGL task or while holding
 *     lvgl_port_lock(). The SIP/RTP/audio tasks never call LVGL.
 *   - The render task runs below the audio tasks so a screen refresh cannot
 *     stall a 20 ms RTP frame.
 *   - Flush is synchronous: the task waits for the QSPI transfer completion
 *     before handing the buffer back to LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY   3

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel);

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
