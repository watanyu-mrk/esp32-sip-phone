#include "phone_control.h"

#include <string.h>

#include "button.h"
#include "config_manager.h"
#include "esp_log.h"
#include "sip_client.h"

static const char *TAG = "PHONE";

static bool s_initialised;

#if CONFIG_SIP_PHONE_LEGACY_BUTTONS
static void on_button(button_id_t id, button_event_t event)
{
    if (event == BUTTON_EVENT_RELEASED) {
        return;
    }

    if (id == BUTTON_ANSWER) {
        sip_call_state_t state = sip_client_get_call_state();

        if (state == SIP_CALL_RINGING_INCOMING) {
            ESP_LOGI(TAG, "[BUTTON] ANSWER pressed");
            esp_err_t err = sip_client_answer();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[PHONE] Answer failed: %s", esp_err_to_name(err));
            }
            return;
        }

        if (state == SIP_CALL_IDLE) {
            const phone_config_t *cfg = config_manager_get();
            const char *number = cfg->phone.default_number;
            if (number[0] == '\0') {
                ESP_LOGW(TAG, "[PHONE] ANSWER pressed but no default number is configured");
                return;
            }
            ESP_LOGI(TAG, "[BUTTON] ANSWER pressed, dialling %s", number);
            esp_err_t err = sip_client_dial(number);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[PHONE] Dial failed: %s", esp_err_to_name(err));
            }
            return;
        }

        ESP_LOGD(TAG, "[PHONE] ANSWER ignored in state %d", (int)state);
        return;
    }

    /* HANGUP: rejects a ringing call, cancels dialling, or ends the call. */
    ESP_LOGI(TAG, "[BUTTON] HANGUP pressed");
    esp_err_t err = sip_client_hangup();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "[PHONE] Nothing to hang up (%s)", esp_err_to_name(err));
    }
}
#endif /* CONFIG_SIP_PHONE_LEGACY_BUTTONS */

esp_err_t phone_control_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

#if CONFIG_SIP_PHONE_LEGACY_BUTTONS
    esp_err_t err = button_init(on_button);
    if (err != ESP_OK) {
        return err;
    }
#else
    ESP_LOGI(TAG, "[PHONE] Physical buttons disabled (touch UI build)");
#endif

    s_initialised = true;
    ESP_LOGI(TAG, "[PHONE] Ready");
    return ESP_OK;
}

esp_err_t phone_control_dial(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return sip_client_dial(number);
}

esp_err_t phone_control_answer(void)
{
    return sip_client_answer();
}

esp_err_t phone_control_hangup(void)
{
    return sip_client_hangup();
}

esp_err_t phone_control_reject(void)
{
    return sip_client_hangup();
}

esp_err_t phone_control_apply_settings(void)
{
    return ESP_OK;
}

bool phone_control_is_in_call(void)
{
    return sip_client_get_call_state() == SIP_CALL_ESTABLISHED;
}
