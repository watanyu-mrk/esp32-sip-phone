/*
 * Call orchestration: maps button/web intents onto SIP + RTP actions and owns
 * the call state machine (idle, dialing, ringing, in-call, teardown).
 *
 * Ownership rules: this module starts and stops the audio session exactly once
 * per call and releases mic/speaker/RTP resources on every termination path.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t phone_control_init(void);

esp_err_t phone_control_dial(const char *number);
esp_err_t phone_control_answer(void);
esp_err_t phone_control_hangup(void);
esp_err_t phone_control_reject(void);

/* Auto-answer, ring timeout, ringtone and call timeout policy from
 * configuration. */
esp_err_t phone_control_apply_settings(void);

/* Plays one ringtone cycle so the selected tone can be heard. Only valid when
 * the phone is idle. */
esp_err_t phone_control_preview_ringtone(uint8_t tone, uint8_t volume);

bool phone_control_is_in_call(void);
