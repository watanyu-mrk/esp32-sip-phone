/*
 * NV3041A QSPI panel driver for esp_lcd.
 *
 * The board's panel is addressed with the 32-bit QSPI DBI framing used by
 * other QSPI controllers: the command phase is one 32-bit word
 * (0x02 << 24 | cmd << 8) and pixels are written with the 0x32 write opcode.
 * Initialisation values come from the JC4827W543 vendor/ESPHome sequence;
 * they are panel specific, not a generic NV3041A default.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} nv3041_lcd_init_cmd_t;

esp_err_t esp_lcd_new_panel_nv3041(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);
