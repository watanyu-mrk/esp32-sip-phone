/*
 * Physical buttons: ANSWER on GPIO3 (strapping pin) and HANGUP on GPIO2.
 *
 * Wiring constraints before this is implemented:
 *   - GPIO3 must read high at boot; do not add a pull-down that breaks boot.
 *   - Use the ESP32-S3 internal pull-ups plus debounce (BUTTON_DEBOUNCE_MS).
 *   - A long press may be mapped to reject/redial; keep it optional.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define BUTTON_ANSWER_GPIO   3 /* strapping pin: must stay boot-safe when held */
#define BUTTON_HANGUP_GPIO   2
#define BUTTON_DEBOUNCE_MS   30
#define BUTTON_LONG_PRESS_MS 1500

typedef enum {
    BUTTON_ANSWER = 0,
    BUTTON_HANGUP,
} button_id_t;

typedef enum {
    BUTTON_EVENT_PRESSED = 0,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_LONG_PRESS,
} button_event_t;

typedef void (*button_callback_t)(button_id_t id, button_event_t event);

/* Registers the GPIOs and starts the button task. Callback runs in that task;
 * keep it short and defer heavy work to queues. */
esp_err_t button_init(button_callback_t callback);
