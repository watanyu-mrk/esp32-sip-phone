#include "phone_control.h"

#include "esp_log.h"

static const char *TAG = "PHONE";

/*
 * Scaffold stub. Call control depends on a real SIP stack (see sip_client), so
 * every intent is refused instead of pretending to start audio.
 */
esp_err_t phone_control_init(void)
{
    ESP_LOGW(TAG, "[PHONE] init stub: call state machine not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t phone_control_dial(const char *number)
{
    ESP_LOGW(TAG, "[PHONE] dial stub: '%s' not dialled", (number != NULL) ? number : "");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t phone_control_answer(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t phone_control_hangup(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t phone_control_reject(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t phone_control_apply_settings(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool phone_control_is_in_call(void)
{
    return false;
}
