/*
 * RTP transport for G.711 audio plus RFC 4733 DTMF events.
 * Phase 4: bind a local UDP port from RTP_LOCAL_PORT_MIN..MAX, send 20 ms
 * packets, receive and push into the jitter buffer, track loss/jitter stats.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RTP_PAYLOAD_TYPE_PCMU 0
#define RTP_PAYLOAD_TYPE_PCMA 8

#define RTP_LOCAL_PORT_MIN 20000
#define RTP_LOCAL_PORT_MAX 20100
#define RTP_JITTER_FRAMES  10

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t packets_out_of_order;
    uint32_t jitter_ms;
} rtp_stats_t;

esp_err_t rtp_client_init(void);

/* Binds the local RTP port from RTP_LOCAL_PORT_MIN..MAX and reports it so the
 * SIP layer can put the same port in the SDP offer/answer. Idempotent. */
esp_err_t rtp_client_open(uint16_t *local_port);

/*
 * payload_type: RTP_PAYLOAD_TYPE_PCMU (0) or RTP_PAYLOAD_TYPE_PCMA (8).
 * Local port is chosen from RTP_LOCAL_PORT_MIN..MAX.
 */
esp_err_t rtp_client_start(const char *remote_ip, uint16_t remote_port, uint8_t payload_type);

esp_err_t rtp_client_stop(void);

esp_err_t rtp_client_send_pcm(const int16_t *samples, size_t sample_count);

esp_err_t rtp_client_get_stats(rtp_stats_t *out);
