/*
 * GUITION JC4827W543 display: 4.3" 480x272 IPS panel over quad SPI.
 *
 * The panel controller is NV3041A (Guition's brief calls it ST3401A; both
 * names appear in vendor and community material for this board). The QSPI
 * wiring below is the one used by the board's ESPHome/ESP-IDF ports:
 *
 *   CS=45  SCK=47  D0=21  D1=48  D2=40  D3=39  backlight=1
 *
 * There is no dedicated LCD reset line on the schematic; the controller is
 * reset with the software reset command (0x01).
 */
#pragma once

#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#define LCD_H_RES 480
#define LCD_V_RES 272
#define LCD_BITS_PER_PIXEL 16

#define LCD_HOST           SPI3_HOST
#define LCD_QSPI_CS_GPIO   45
#define LCD_QSPI_SCK_GPIO  47
#define LCD_QSPI_D0_GPIO   21
#define LCD_QSPI_D1_GPIO   48
#define LCD_QSPI_D2_GPIO   40
#define LCD_QSPI_D3_GPIO   39

#define LCD_BACKLIGHT_GPIO 1

/* QSPI pixel clock. The panel datasheet allows more, but 20 MHz is the value
 * proven by the board's own ports and keeps the 480x272 frame inside DMA
 * budget. */
#define LCD_QSPI_PCLK_HZ (20 * 1000 * 1000)

/* Pixel format used by the controller and LVGL. */
#define LCD_COLOR_FORMAT_RGB565 1

esp_err_t display_driver_init(void);

/* Non-NULL after a successful display_driver_init(). */
esp_lcd_panel_handle_t display_driver_panel(void);

/* Panel IO handle, used by the LVGL port to receive transfer-complete events. */
esp_lcd_panel_io_handle_t display_driver_panel_io(void);

/* 0 = off, 100 = full brightness. Uses the LEDC PWM channel on GPIO1. */
esp_err_t display_driver_set_backlight(uint8_t percent);
uint8_t display_driver_get_backlight(void);
