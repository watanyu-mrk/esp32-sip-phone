#include "sip_client.h"

#include "esp_log.h"

static const char *TAG = "SIP";

/*
 * Scaffold stub. No SIP stack is linked in yet, so every operation reports
 * ESP_ERR_NOT_SUPPORTED. Per project rule 5, no fake REGISTER/INVITE dance is
 * performed here: the logs state plainly that nothing was sent.
 */
static sip_reg_state_t s_reg_state = SIP_REG_STATE_UNREGISTERED;
static sip_call_state_t s_call_state = SIP_CALL_IDLE;

esp_err_t sip_client_init(void)
{
    ESP_LOGW(TAG, "[SIP] init stub: no SIP stack linked (PJSIP or equivalent pending)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sip_client_register(void)
{
    ESP_LOGW(TAG, "[SIP] register stub: REGISTER not sent, stack missing");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sip_client_unregister(void)
{
    ESP_LOGW(TAG, "[SIP] unregister stub: nothing to unregister");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sip_client_dial(const char *number)
{
    ESP_LOGW(TAG, "[SIP] dial stub: INVITE to '%s' not sent", (number != NULL) ? number : "");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sip_client_answer(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sip_client_hangup(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

sip_reg_state_t sip_client_get_reg_state(void)
{
    return s_reg_state;
}

sip_call_state_t sip_client_get_call_state(void)
{
    return s_call_state;
}
