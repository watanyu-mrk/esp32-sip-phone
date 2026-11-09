#include "app_state.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SYSTEM";

static app_state_t s_state;
static SemaphoreHandle_t s_lock;
static int64_t s_call_started_us;

void app_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.phone_state = PHONE_STATE_IDLE;
    s_state.wifi_rssi = -127;
    s_call_started_us = 0;

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "[SYSTEM] app_state: failed to create mutex");
    }
}

static bool app_state_lock(void)
{
    return (s_lock != NULL) && (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE);
}

static void app_state_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

app_state_t app_state_get(void)
{
    app_state_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!app_state_lock()) {
        return snapshot;
    }

    snapshot = s_state;
    int64_t now_us = esp_timer_get_time();
    snapshot.uptime_s = (uint32_t)(now_us / 1000000LL);
    if (s_call_started_us > 0 && s_state.phone_state == PHONE_STATE_IN_CALL) {
        snapshot.call_duration_s = (uint32_t)((now_us - s_call_started_us) / 1000000LL);
    }

    app_state_unlock();
    return snapshot;
}

void app_state_set_wifi(bool connected, int8_t rssi)
{
    if (!app_state_lock()) {
        return;
    }
    s_state.wifi_connected = connected;
    s_state.wifi_rssi = rssi;
    app_state_unlock();
}

void app_state_set_sip_registered(bool registered)
{
    if (!app_state_lock()) {
        return;
    }
    s_state.sip_registered = registered;
    app_state_unlock();
}

void app_state_set_phone_state(phone_state_t state)
{
    if (!app_state_lock()) {
        return;
    }

    s_state.phone_state = state;
    if (state == PHONE_STATE_IN_CALL) {
        if (s_call_started_us == 0) {
            s_call_started_us = esp_timer_get_time();
        }
    } else {
        s_call_started_us = 0;
        s_state.call_duration_s = 0;
    }

    app_state_unlock();
}

void app_state_set_remote_number(const char *number)
{
    if (!app_state_lock()) {
        return;
    }
    memset(s_state.remote_number, 0, sizeof(s_state.remote_number));
    if (number != NULL) {
        strncpy(s_state.remote_number, number, sizeof(s_state.remote_number) - 1);
    }
    app_state_unlock();
}

const char *app_state_phone_state_name(phone_state_t state)
{
    switch (state) {
    case PHONE_STATE_IDLE:             return "IDLE";
    case PHONE_STATE_REGISTERING:      return "REGISTERING";
    case PHONE_STATE_REGISTERED:       return "REGISTERED";
    case PHONE_STATE_DIALING:          return "DIALING";
    case PHONE_STATE_RINGING_OUTGOING: return "RINGING_OUTGOING";
    case PHONE_STATE_RINGING_INCOMING: return "RINGING_INCOMING";
    case PHONE_STATE_IN_CALL:          return "CONNECTED";
    case PHONE_STATE_HANGING_UP:       return "HANGING_UP";
    case PHONE_STATE_ERROR:            return "ERROR";
    default:                           return "UNKNOWN";
    }
}
