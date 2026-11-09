/*
 * Shared, thread-safe view of the phone's runtime state.
 * Readers (web server, monitor task, logging) use app_state_get(), which
 * returns a copy so no lock is held by callers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

typedef enum {
    PHONE_STATE_IDLE = 0,
    PHONE_STATE_REGISTERING,
    PHONE_STATE_REGISTERED,
    PHONE_STATE_DIALING,
    PHONE_STATE_RINGING_OUTGOING,
    PHONE_STATE_RINGING_INCOMING,
    PHONE_STATE_IN_CALL,
    PHONE_STATE_HANGING_UP,
    PHONE_STATE_ERROR,
} phone_state_t;

typedef struct {
    bool wifi_connected;
    int8_t wifi_rssi;
    bool sip_registered;
    phone_state_t phone_state;
    char remote_number[APP_REMOTE_NUMBER_LEN];
    uint32_t call_duration_s;
    uint32_t uptime_s;
} app_state_t;

void app_state_init(void);

/* Snapshot copy; uptime and call duration are refreshed at read time. */
app_state_t app_state_get(void);

void app_state_set_wifi(bool connected, int8_t rssi);
void app_state_set_sip_registered(bool registered);
void app_state_set_phone_state(phone_state_t state);
void app_state_set_remote_number(const char *number);

const char *app_state_phone_state_name(phone_state_t state);
