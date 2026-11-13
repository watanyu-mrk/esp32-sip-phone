#include "phone_control.h"

#include <string.h>

#include "button.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ringtone.h"
#include "sip_client.h"

static const char *TAG = "PHONE";

/* Call duration cap, ring timeout and ringtone playback from the Phone
 * settings. The watchdog runs in the esp_timer task, so it never blocks the UI
 * or the SIP task; 200 ms keeps the ringtone start close to the INVITE. */
#define PHONE_WATCH_PERIOD_MS 200

static bool s_initialised;
static uint16_t s_call_timeout_s;
static uint16_t s_ring_timeout_s;
static int64_t s_call_started_ms;
static int64_t s_ring_started_ms;
static esp_timer_handle_t s_watch_timer;

#if CONFIG_SIP_PHONE_LEGACY_BUTTONS
static void on_button(button_id_t id, button_event_t event)
{
    if (event == BUTTON_EVENT_RELEASED) {
        return;
    }

    if (id == BUTTON_ANSWER) {
        sip_call_state_t state = sip_client_get_call_state();

        if (state == SIP_CALL_RINGING_INCOMING) {
            ESP_LOGI(TAG, "[BUTTON] ANSWER pressed");
            esp_err_t err = sip_client_answer();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[PHONE] Answer failed: %s", esp_err_to_name(err));
            }
            return;
        }

        if (state == SIP_CALL_IDLE) {
            const phone_config_t *cfg = config_manager_get();
            const char *number = cfg->phone.default_number;
            if (number[0] == '\0') {
                ESP_LOGW(TAG, "[PHONE] ANSWER pressed but no default number is configured");
                return;
            }
            ESP_LOGI(TAG, "[BUTTON] ANSWER pressed, dialling %s", number);
            esp_err_t err = sip_client_dial(number);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[PHONE] Dial failed: %s", esp_err_to_name(err));
            }
            return;
        }

        ESP_LOGD(TAG, "[PHONE] ANSWER ignored in state %d", (int)state);
        return;
    }

    /* HANGUP: rejects a ringing call, cancels dialling, or ends the call. */
    ESP_LOGI(TAG, "[BUTTON] HANGUP pressed");
    esp_err_t err = sip_client_hangup();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "[PHONE] Nothing to hang up (%s)", esp_err_to_name(err));
    }
}
#endif /* CONFIG_SIP_PHONE_LEGACY_BUTTONS */

/* Enforces the configured talk-time cap and ring timeout. Both clocks are
 * driven from this watchdog and are dropped as soon as the call leaves the
 * state they measure. */
/* A looped ringtone only makes sense while an INVITE is still ringing. */
static bool ring_gate(void)
{
    return sip_client_get_call_state() == SIP_CALL_RINGING_INCOMING;
}

/* The call audio holds the speaker from answer until the call is released, so
 * a finishing tone must not switch the output path off. */
static bool call_audio_busy(void)
{
    sip_call_state_t state = sip_client_get_call_state();
    return (state == SIP_CALL_ESTABLISHED) || (state == SIP_CALL_TERMINATING);
}

static void watch_timer_cb(void *arg)
{
    (void)arg;

    sip_call_state_t state = sip_client_get_call_state();
    int64_t now = esp_timer_get_time() / 1000;

    /* Talk time: counted from the moment the call is established. */
    if (state != SIP_CALL_ESTABLISHED) {
        s_call_started_ms = 0;
    } else if (s_call_started_ms == 0) {
        s_call_started_ms = now;
        if (s_call_timeout_s > 0) {
            ESP_LOGI(TAG, "[PHONE] Call timer started (%us limit)",
                     (unsigned)s_call_timeout_s);
        }
    } else if (s_call_timeout_s > 0 &&
               now - s_call_started_ms >= (int64_t)s_call_timeout_s * 1000) {
        ESP_LOGW(TAG, "[PHONE] Call timeout reached (%us), hanging up",
                 (unsigned)s_call_timeout_s);
        s_call_started_ms = 0;
        esp_err_t err = sip_client_hangup();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[PHONE] Timeout hangup failed: %s", esp_err_to_name(err));
        }
        return;
    }

    /* Ring time: an unanswered incoming call is rejected and an outgoing call
     * that is never answered is cancelled, so a call cannot ring forever. */
    bool ringing = (state == SIP_CALL_DIALING) ||
                   (state == SIP_CALL_RINGING_OUTGOING) ||
                   (state == SIP_CALL_RINGING_INCOMING);
    if (!ringing) {
        s_ring_started_ms = 0;
        /* Only a ring is stopped here. A one-shot preview from the settings
         * page is allowed to run to its end. */
        if (ringtone_is_ringing()) {
            /* Answered: the call audio takes the speaker over, so only the
             * ringtone must stop. Any other exit also owns the output path. */
            ringtone_stop(state != SIP_CALL_ESTABLISHED);
        }
        return;
    }

    /* Only an incoming call rings locally; for outgoing calls the ringback
     * tone comes from the network. */
    if (state == SIP_CALL_RINGING_INCOMING && !ringtone_is_ringing()) {
        const phone_config_t *cfg = config_manager_get();
        if (cfg != NULL) {
            if (ringtone_is_playing()) {
                /* A preview is still running: the call wins. */
                ringtone_stop(false);
            }
            esp_err_t err = ringtone_start(cfg->ring.ringtone, cfg->ring.ring_volume);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[PHONE] Ringtone start failed: %s", esp_err_to_name(err));
            }
        }
    }

    if (s_ring_started_ms == 0) {
        s_ring_started_ms = now;
        if (s_ring_timeout_s > 0) {
            ESP_LOGI(TAG, "[PHONE] Ring timer started (%us limit)",
                     (unsigned)s_ring_timeout_s);
        }
        return;
    }

    if (s_ring_timeout_s == 0 ||
        now - s_ring_started_ms < (int64_t)s_ring_timeout_s * 1000) {
        return;
    }

    s_ring_started_ms = 0;
    ESP_LOGW(TAG, "[PHONE] Ring timeout reached (%us), %s",
             (unsigned)s_ring_timeout_s,
             (state == SIP_CALL_RINGING_INCOMING) ? "rejecting call" : "cancelling call");
    esp_err_t err = sip_client_hangup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[PHONE] Ring timeout hangup failed: %s", esp_err_to_name(err));
    }
}

esp_err_t phone_control_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

#if CONFIG_SIP_PHONE_LEGACY_BUTTONS
    esp_err_t err = button_init(on_button);
    if (err != ESP_OK) {
        return err;
    }
#else
    ESP_LOGI(TAG, "[PHONE] Physical buttons disabled (touch UI build)");
#endif

    esp_err_t rerr = ringtone_init();
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "[PHONE] Ringtone engine unavailable: %s", esp_err_to_name(rerr));
    }
    ringtone_set_gate(ring_gate);
    ringtone_set_busy_fn(call_audio_busy);

    s_initialised = true;
    phone_control_apply_settings();

    if (s_watch_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = watch_timer_cb,
            .name = "call_watch",
        };
        esp_err_t terr = esp_timer_create(&args, &s_watch_timer);
        if (terr != ESP_OK) {
            s_initialised = false;
            ESP_LOGE(TAG, "[PHONE] Failed to create call watchdog: %s",
                     esp_err_to_name(terr));
            return terr;
        }
        esp_timer_start_periodic(s_watch_timer, PHONE_WATCH_PERIOD_MS * 1000);
    }

    ESP_LOGI(TAG, "[PHONE] Ready");
    return ESP_OK;
}

esp_err_t phone_control_dial(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return sip_client_dial(number);
}

esp_err_t phone_control_answer(void)
{
    esp_err_t err = sip_client_answer();
    if (err == ESP_OK) {
        /* Answered before the watchdog notices: hand the speaker straight to
         * the call audio instead of waiting for the next tick. */
        ringtone_stop(false);
    }
    return err;
}

/* Rejecting and hanging up both end the call; when it was still ringing the
 * ringtone also owns the speaker and has to release it. */
static esp_err_t hangup_common(void)
{
    bool ringing = (sip_client_get_call_state() == SIP_CALL_RINGING_INCOMING);
    esp_err_t err = sip_client_hangup();
    if (ringing) {
        ringtone_stop(true);
    }
    return err;
}

esp_err_t phone_control_hangup(void)
{
    return hangup_common();
}

esp_err_t phone_control_reject(void)
{
    return hangup_common();
}

esp_err_t phone_control_preview_ringtone(uint8_t tone, uint8_t volume)
{
    if (sip_client_get_call_state() != SIP_CALL_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ringtone_preview((int)tone, volume);
}

esp_err_t phone_control_apply_settings(void)
{
    const phone_config_t *cfg = config_manager_get();
    if (cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_call_timeout_s = cfg->phone.call_timeout_s;
    s_ring_timeout_s = cfg->phone.ring_duration_s;
    s_call_started_ms = 0;
    s_ring_started_ms = 0;

    if (s_call_timeout_s == 0) {
        ESP_LOGI(TAG, "[PHONE] Call timeout: off, ring timeout: %us",
                 (unsigned)s_ring_timeout_s);
    } else {
        ESP_LOGI(TAG, "[PHONE] Call timeout: %us, ring timeout: %us",
                 (unsigned)s_call_timeout_s, (unsigned)s_ring_timeout_s);
    }
    ESP_LOGI(TAG, "[PHONE] Ring tone: %s at %u%%, key tone %u%%, call volume %u%%",
             ringtone_name(cfg->ring.ringtone), (unsigned)cfg->ring.ring_volume,
             (unsigned)cfg->keytone.key_volume, (unsigned)cfg->audio.speaker_volume);
    return ESP_OK;
}

bool phone_control_is_in_call(void)
{
    return sip_client_get_call_state() == SIP_CALL_ESTABLISHED;
}
