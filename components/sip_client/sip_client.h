/*
 * SIP signalling facade.
 *
 * The SIP stack has not been selected/vendored yet. Candidate: PJSIP built for
 * ESP32-S3, or another stack with proven ESP-IDF support. Do not hand-roll a
 * minimal SIP parser to stand in for it (project rule).
 *
 * Required behaviour once implemented: REGISTER with digest auth, re-register
 * before expiry, INVITE/ACK/CANCEL/BYE, 100/180/183/200 handling, OPTIONS
 * keepalive, RFC 4733 DTMF, call state machine feeding app_state.
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
