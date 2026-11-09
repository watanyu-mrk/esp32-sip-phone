/*
 * Fixed-size jitter buffer for inbound audio frames.
 * Phase 4: order by RTP sequence number, drop duplicates and very late
 * packets, emit silence on gaps, report mismatch for SDP-negotiated payload.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct rtp_jitter_buffer rtp_jitter_buffer_t;

rtp_jitter_buffer_t *rtp_jitter_create(size_t max_frames);
void rtp_jitter_destroy(rtp_jitter_buffer_t *jb);

esp_err_t rtp_jitter_push(rtp_jitter_buffer_t *jb,
                          uint16_t seq,
                          uint32_t timestamp,
                          const uint8_t *payload,
                          size_t payload_len);

/* Copies the next in-order frame into out. Returns the byte count, 0 when the
 * buffer needs to emit silence, or 0 if empty (check rtp_jitter_is_empty). */
size_t rtp_jitter_pop(rtp_jitter_buffer_t *jb, uint8_t *out, size_t out_len);

bool rtp_jitter_is_empty(const rtp_jitter_buffer_t *jb);
