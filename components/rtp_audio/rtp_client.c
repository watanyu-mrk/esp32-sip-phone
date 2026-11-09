#include "rtp_client.h"

#include "esp_log.h"

static const char *TAG = "RTP";

/*
 * Scaffold stub: no socket is opened and no packet is transmitted, so there is
 * no audio path to misreport as working.
 */
esp_err_t rtp_client_init(void)
{
    ESP_LOGW(TAG, "[RTP] init stub: UDP transport not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtp_client_start(const char *remote_ip, uint16_t remote_port, uint8_t payload_type)
{
    ESP_LOGW(TAG, "[RTP] start stub: %s:%u pt=%u not started",
             (remote_ip != NULL) ? remote_ip : "", (unsigned)remote_port, (unsigned)payload_type);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtp_client_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtp_client_send_pcm(const int16_t *samples, size_t sample_count)
{
    (void)samples;
    (void)sample_count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtp_client_get_stats(rtp_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (rtp_stats_t){0};
    return ESP_ERR_NOT_SUPPORTED;
}
