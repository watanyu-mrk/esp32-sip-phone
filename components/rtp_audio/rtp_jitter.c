#include "rtp_jitter.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "RTP";

struct rtp_jitter_buffer {
    int placeholder; /* replaced by the frame ring in Phase 4 */
};

/*
 * Scaffold stub: the buffer is not allocated so callers cannot believe packets
 * are being reordered yet. rtp_jitter_create() returns NULL by design.
 */
rtp_jitter_buffer_t *rtp_jitter_create(size_t max_frames)
{
    ESP_LOGW(TAG, "[RTP] jitter buffer stub: would allocate %u frames",
             (unsigned)max_frames);
    return NULL;
}

void rtp_jitter_destroy(rtp_jitter_buffer_t *jb)
{
    (void)jb;
}

esp_err_t rtp_jitter_push(rtp_jitter_buffer_t *jb,
                          uint16_t seq,
                          uint32_t timestamp,
                          const uint8_t *payload,
                          size_t payload_len)
{
    (void)jb;
    (void)seq;
    (void)timestamp;
    (void)payload;
    (void)payload_len;
    return ESP_ERR_NOT_SUPPORTED;
}

size_t rtp_jitter_pop(rtp_jitter_buffer_t *jb, uint8_t *out, size_t out_len)
{
    (void)jb;
    (void)out;
    (void)out_len;
    return 0;
}

bool rtp_jitter_is_empty(const rtp_jitter_buffer_t *jb)
{
    (void)jb;
    return true;
}
