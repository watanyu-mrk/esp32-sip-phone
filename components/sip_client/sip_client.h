/*
 * SIP user agent facade.
 *
 * The pre-compiled esp_sip_service/esp_rtc stack was rejected on hardware:
 * after the first REGISTER it re-reads the same buffered response in a tight
 * loop, never sends the authenticated retry and starves CPU0 until the task
 * watchdog kills the web UI and audio. sip_client.c therefore implements the
 * required SIP subset directly on lwIP (REGISTER with digest auth, INVITE,
 * ACK, CANCEL, BYE, OPTIONS and SDP negotiation for G.711). It is a real
 * protocol implementation talking to the PBX, not a simulator.
 *
 * Media: rtp_audio owns the RTP socket and the G.711 payload; the SIP layer
 * only negotiates the remote address, port and payload type.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SIP_REG_STATE_UNREGISTERED = 0,
    SIP_REG_STATE_REGISTERING,
    SIP_REG_STATE_REGISTERED,
    SIP_REG_STATE_FAILED,
} sip_reg_state_t;

typedef enum {
    SIP_CALL_IDLE = 0,
    SIP_CALL_DIALING,
    SIP_CALL_RINGING_OUTGOING,
    SIP_CALL_RINGING_INCOMING,
    SIP_CALL_ESTABLISHED,
    SIP_CALL_TERMINATING,
} sip_call_state_t;

esp_err_t sip_client_init(void);

esp_err_t sip_client_register(void);
esp_err_t sip_client_unregister(void);
esp_err_t sip_client_dial(const char *number);
esp_err_t sip_client_answer(void);
esp_err_t sip_client_hangup(void);

sip_reg_state_t sip_client_get_reg_state(void);
sip_call_state_t sip_client_get_call_state(void);
