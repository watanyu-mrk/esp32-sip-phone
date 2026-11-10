#include "rtp_jitter.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "RTP";

#define JITTER_PAYLOAD_MAX 200

typedef struct {
    bool used;
    uint16_t seq;
    uint32_t timestamp;
    size_t len;
    uint8_t data[JITTER_PAYLOAD_MAX];
} jitter_frame_t;

struct rtp_jitter_buffer {
    size_t max;
    bool have_next;
    uint16_t next;
    jitter_frame_t *frames;
};

rtp_jitter_buffer_t *rtp_jitter_create(size_t max_frames)
{
    if (max_frames < 2) {
        max_frames = 2;
    }

    rtp_jitter_buffer_t *jb = calloc(1, sizeof(*jb));
    if (jb == NULL) {
        return NULL;
    }
    jb->frames = calloc(max_frames, sizeof(*jb->frames));
    if (jb->frames == NULL) {
        free(jb);
        return NULL;
    }
    jb->max = max_frames;

    ESP_LOGI(TAG, "[RTP] Jitter buffer: %u frames (%u ms)", (unsigned)max_frames,
             (unsigned)(max_frames * 20));
    return jb;
}

void rtp_jitter_destroy(rtp_jitter_buffer_t *jb)
{
    if (jb == NULL) {
        return;
    }
    free(jb->frames);
    free(jb);
}

esp_err_t rtp_jitter_push(rtp_jitter_buffer_t *jb,
                          uint16_t seq,
                          uint32_t timestamp,
                          const uint8_t *payload,
                          size_t payload_len)
{
    if (jb == NULL || payload == NULL || payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!jb->have_next) {
        jb->next = seq;
        jb->have_next = true;
    }

    int16_t offset = (int16_t)(seq - jb->next);
    if (offset < 0 || (size_t)offset >= jb->max) {
        return ESP_OK; /* late or too far ahead: drop */
    }
    if (payload_len > JITTER_PAYLOAD_MAX) {
        payload_len = JITTER_PAYLOAD_MAX;
    }

    jitter_frame_t *f = &jb->frames[offset];
    if (f->used && f->seq == seq) {
        return ESP_OK; /* duplicate */
    }

    f->used = true;
    f->seq = seq;
    f->timestamp = timestamp;
    f->len = payload_len;
    memcpy(f->data, payload, payload_len);
    return ESP_OK;
}

size_t rtp_jitter_pop(rtp_jitter_buffer_t *jb, uint8_t *out, size_t out_len)
{
    if (jb == NULL || out == NULL || out_len == 0 || !jb->have_next) {
        return 0;
    }

    jitter_frame_t *f = &jb->frames[0];
    size_t copied = 0;

    if (f->used) {
        copied = (f->len < out_len) ? f->len : out_len;
        memcpy(out, f->data, copied);
    }

    bool any = false;
    for (size_t i = 1; i < jb->max; i++) {
        if (jb->frames[i].used) {
            any = true;
            break;
        }
    }

    memmove(&jb->frames[0], &jb->frames[1], sizeof(jb->frames[0]) * (jb->max - 1));
    memset(&jb->frames[jb->max - 1], 0, sizeof(jb->frames[0]));
    jb->next++;

    if (!any) {
        jb->have_next = false;
    }
    return copied;
}

bool rtp_jitter_is_empty(const rtp_jitter_buffer_t *jb)
{
    if (jb == NULL || !jb->have_next) {
        return true;
    }
    for (size_t i = 0; i < jb->max; i++) {
        if (jb->frames[i].used) {
            return false;
        }
    }
    return true;
}
