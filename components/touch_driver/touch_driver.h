/*
 * GT911 capacitive touch on the JC4827W543.
 *
 * I2C: SDA=8, SCL=4, INT=3, RST=38 (address 0x5D, 0x14 as fallback).
 * The controller is polled through register 0x814E; the INT line is only used
 * for the reset/address strap and is left as an input afterwards.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TOUCH_I2C_SDA_GPIO 8
#define TOUCH_I2C_SCL_GPIO 4
#define TOUCH_INT_GPIO     3
#define TOUCH_RST_GPIO     38

#define TOUCH_I2C_ADDR_PRIMARY   0x5D
#define TOUCH_I2C_ADDR_ALTERNATE 0x14
#define TOUCH_I2C_FREQ_HZ        400000

#define TOUCH_H_RES 272
#define TOUCH_V_RES 480

/* The GT911 reports in the panel's native 480x272 landscape frame. The UI and
 * the panel MADCTL are portrait (MV|MY), so raw (x,y) becomes screen (y, 479-x).
 * Tune these three macros if touch lands mirrored or axis-swapped on a
 * different board revision. */
#define TOUCH_SWAP_XY 1
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 1

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} touch_point_t;

esp_err_t touch_driver_init(void);

/* Reads the controller and clears its buffer. Always returns true once the
 * controller is ready and fills the latest known state (held touches included),
 * so callers never have to invent a release. */
bool touch_driver_read(touch_point_t *point);

/* True when the GT911 answered with its product ID during init. */
bool touch_driver_is_ready(void);
