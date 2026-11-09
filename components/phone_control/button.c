#include "button.h"

#include "esp_log.h"

static const char *TAG = "BUTTON";

/*
 * Scaffold stub: no GPIO configured and no button task created, so a held
 * ANSWER button cannot interfere with boot during this phase.
 */
esp_err_t button_init(button_callback_t callback)
{
    (void)callback;
    ESP_LOGW(TAG, "[BUTTON] init stub: GPIO%d/GPIO%d not configured yet",
             BUTTON_ANSWER_GPIO, BUTTON_HANGUP_GPIO);
    return ESP_ERR_NOT_SUPPORTED;
}
