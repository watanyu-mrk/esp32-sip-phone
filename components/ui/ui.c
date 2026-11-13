/*
 * Portrait home screen for the 272x480 touch panel.
 *
 * Layout: status header, a framed display card for the dialled number, then a
 * 3x4 keypad with the call actions underneath. The CALL button hands the
 * number to phone_control (the call orchestrator), never to the SIP stack, and
 * reports the real error when a call cannot start.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "config_manager.h"
#include "ringtone.h"
#include "phone_control.h"
#include "sip_client.h"
#include "ui.h"
#include "ui_settings.h"
#include "wifi_manager.h"

static const char *TAG = "UI";

#define COL_BG       0x101A2B
#define COL_HEADER   0x0B1220
#define COL_CARD     0x1B2A44
#define COL_CARD_HI  0x26395A
#define COL_TEXT     0xE8EEF7
#define COL_DIM      0x93A4BF
#define COL_PRIMARY  0x2F6FED
#define COL_GREEN    0x27AE60
#define COL_RED      0xE74C3C
#define COL_WARN     0xF39C12

#define DIAL_MAX_LEN 24

/* Portrait geometry (272 wide, 480 tall). */
#define HEADER_H     30
#define CARD_X       8
#define CARD_Y       36
#define CARD_W       256
#define CARD_H       84
#define KEY_X0       8
#define KEY_Y0       128
#define KEY_W        80
#define KEY_H        60
#define KEY_GAP      8
#define ACTION_Y     404
#define ACTION_W     124
#define ACTION_H     56

static lv_obj_t *s_ext_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_sip_label;
static lv_obj_t *s_number_label;
static lv_obj_t *s_delete_btn;
static lv_obj_t *s_call_btn;
static lv_obj_t *s_answer_btn;
static lv_obj_t *s_reject_btn;
static lv_obj_t *s_hangup_btn;

static char s_number[DIAL_MAX_LEN + 1];
static char s_notice[48];
static uint32_t s_notice_until;

static void style_button(lv_obj_t *btn, uint32_t color, uint32_t pressed_color,
                         uint32_t text_color, const lv_font_t *font)
{
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(pressed_color), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(btn, font, 0);
    lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, 0);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                             uint32_t color, uint32_t pressed, uint32_t text_color,
                             const lv_font_t *font, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    /* Strip the theme styles first: lv_obj_set_pos/size write into a local
     * style, so removing styles afterwards deletes the geometry and the
     * button collapses to zero size together with its label. */
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    style_button(btn, color, pressed, text_color, font);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    /* The theme gives a new label its own font, so set both explicitly;
     * otherwise every key falls back to the 14 px default. */
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static void show_notice(const char *text, uint32_t color)
{
    snprintf(s_notice, sizeof(s_notice), "%s", text);
    s_notice_until = lv_tick_get() + 2500;
    lv_obj_set_style_text_color(s_number_label, lv_color_hex(color), 0);
    lv_label_set_text(s_number_label, s_notice);
}

static void render_number(void)
{
    if (s_notice_until && (int32_t)(s_notice_until - lv_tick_get()) > 0) {
        return;
    }
    if (s_number[0]) {
        lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(s_number_label, s_number);
    } else {
        lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(s_number_label, "Enter number");
    }
}

static void dial_key_cb(lv_event_t *event)
{
    const char *key = lv_event_get_user_data(event);
    size_t len = strlen(s_number);

    s_notice_until = 0;

    /* Feedback only while dialling. DTMF inside a call belongs on the RTP
     * path; a local tone there would fight the call audio for the speaker. */
    if (sip_client_get_call_state() == SIP_CALL_IDLE) {
        const phone_config_t *cfg = config_manager_get();
        if (cfg != NULL && cfg->keytone.key_volume > 0) {
            esp_err_t err = ringtone_play_key(key[0], cfg->keytone.key_volume);
            if (err != ESP_OK) {
                ESP_LOGD(TAG, "Key tone skipped: %s", esp_err_to_name(err));
            }
        }
    }

    if (len + strlen(key) < sizeof(s_number)) {
        memcpy(&s_number[len], key, strlen(key) + 1);
        render_number();
    }
}

static void delete_cb(lv_event_t *event)
{
    (void)event;
    size_t len = strlen(s_number);
    s_notice_until = 0;
    if (len > 0) {
        s_number[len - 1] = '\0';
    }
    render_number();
}

static void delete_long_cb(lv_event_t *event)
{
    (void)event;
    s_number[0] = '\0';
    s_notice_until = 0;
    render_number();
}

static void menu_cb(lv_event_t *event)
{
    (void)event;
    ui_settings_open();
}

static void call_cb(lv_event_t *event)
{
    (void)event;

    if (s_number[0] == '\0') {
        show_notice("Enter a number", COL_WARN);
        return;
    }
    if (sip_client_get_reg_state() != SIP_REG_STATE_REGISTERED) {
        show_notice("SIP not registered", COL_WARN);
        return;
    }

    esp_err_t err = phone_control_dial(s_number);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Dial failed: %s", esp_err_to_name(err));
        show_notice("Dial failed", COL_RED);
    }
}

static void set_shown(lv_obj_t *obj, bool shown)
{
    if (obj == NULL) {
        return;
    }
    if (shown) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Incoming call: accept it, or decline it with a reason the PBX understands. */
static void answer_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = phone_control_answer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Answer failed: %s", esp_err_to_name(err));
        show_notice("Answer failed", COL_RED);
    }
}

static void reject_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = phone_control_reject();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reject failed: %s", esp_err_to_name(err));
        show_notice("Reject failed", COL_RED);
    }
}

/* Cancels a dialling/ringing call and ends an established one. */
static void hangup_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = phone_control_hangup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hangup failed: %s", esp_err_to_name(err));
        show_notice("Hangup failed", COL_RED);
    }
}

static const char *call_state_text(sip_call_state_t state)
{
    switch (state) {
    case SIP_CALL_DIALING: return "DIALING";
    case SIP_CALL_RINGING_OUTGOING: return "RINGING";
    case SIP_CALL_RINGING_INCOMING: return "INCOMING";
    case SIP_CALL_ESTABLISHED: return "IN CALL";
    case SIP_CALL_TERMINATING: return "ENDING";
    case SIP_CALL_IDLE:
    default: return "IDLE";
    }
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    const phone_config_t *cfg = config_manager_get();
    if (cfg) {
        lv_label_set_text_fmt(s_ext_label, "Ext: %s",
                              cfg->sip.extension[0] ? cfg->sip.extension : "--");
    }

    if (wifi_manager_is_connected()) {
        lv_label_set_text_fmt(s_wifi_label, "WiFi %d", wifi_manager_get_rssi());
        lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COL_TEXT), 0);
    } else {
        lv_label_set_text(s_wifi_label, "WiFi --");
        lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COL_WARN), 0);
    }

    if (sip_client_get_reg_state() == SIP_REG_STATE_REGISTERED) {
        lv_label_set_text(s_sip_label, "SIP ONLINE");
        lv_obj_set_style_text_color(s_sip_label, lv_color_hex(COL_GREEN), 0);
    } else {
        lv_label_set_text(s_sip_label, "SIP OFFLINE");
        lv_obj_set_style_text_color(s_sip_label, lv_color_hex(COL_WARN), 0);
    }

    /* The action row follows the call state: dial controls when idle, answer
     * and reject while an INVITE rings, hangup once a call is up or pending. */
    sip_call_state_t call_state = sip_client_get_call_state();
    bool incoming = (call_state == SIP_CALL_RINGING_INCOMING);
    bool busy = (call_state != SIP_CALL_IDLE) && !incoming;

    set_shown(s_delete_btn, !incoming && !busy);
    set_shown(s_call_btn, !incoming && !busy);
    set_shown(s_answer_btn, incoming);
    set_shown(s_reject_btn, incoming);
    set_shown(s_hangup_btn, busy);

    if (call_state != SIP_CALL_IDLE) {
        /* For an incoming call the interesting number is the caller's, not
         * whatever was left in the dial buffer. */
        char peer[48];
        const char *who = s_number;
        if (incoming) {
            sip_client_get_peer(peer, sizeof(peer));
            who = (peer[0] != '\0') ? peer : "unknown";
        }
        lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text_fmt(s_number_label, "%s  %s", call_state_text(call_state), who);
        return;
    }
    render_number();
}

esp_err_t ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TEXT), 0);

    /* Header: device name on the left, registration state on the right. */
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 272, HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    const phone_config_t *cfg = config_manager_get();
    const char *device_name = (cfg && cfg->system.device_name[0]) ? cfg->system.device_name
                                                                 : "SIP PHONE";

    lv_obj_t *name = lv_label_create(header);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(name, device_name);
    lv_obj_set_pos(name, 6, 7);

    s_sip_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_sip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sip_label, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_text_align(s_sip_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_sip_label, "SIP OFFLINE");
    lv_obj_set_pos(s_sip_label, 154, 6);
    lv_obj_set_size(s_sip_label, 112, 20);

    /* Display card: extension/Wi-Fi strip plus the dialled number. */
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, CARD_X, CARD_Y);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_ext_label = lv_label_create(card);
    lv_obj_set_style_text_font(s_ext_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ext_label, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_ext_label, "Ext: --");
    lv_obj_set_pos(s_ext_label, 10, 6);

    s_wifi_label = lv_label_create(card);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_wifi_label, "WiFi --");
    lv_obj_set_pos(s_wifi_label, 150, 6);
    lv_obj_set_size(s_wifi_label, 96, 20);

    s_number_label = lv_label_create(card);
    lv_obj_set_style_text_font(s_number_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_align(s_number_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_number_label, 10, 32);
    lv_obj_set_size(s_number_label, 236, 44);
    lv_label_set_text(s_number_label, "Enter number");

    /* Keypad: 3x4 keys of 80x60 on a 88x68 pitch. */
    static const char *const keys[4][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {"*", "0", "#"},
    };
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 3; col++) {
            int x = KEY_X0 + col * (KEY_W + KEY_GAP);
            int y = KEY_Y0 + row * (KEY_H + KEY_GAP);
            make_button(screen, x, y, KEY_W, KEY_H, keys[row][col], COL_CARD, COL_CARD_HI,
                        COL_TEXT, &lv_font_montserrat_28, dial_key_cb, (void *)keys[row][col]);
        }
    }

    /* Actions under the keypad. MENU stays put; the rest swap places with the
     * call controls as the call state changes. */
    s_delete_btn = make_button(screen, 8, ACTION_Y, 72, ACTION_H, "DELETE", COL_RED,
                               0xB03A2E, COL_TEXT, &lv_font_montserrat_14, delete_cb, NULL);
    lv_obj_add_event_cb(s_delete_btn, delete_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_call_btn = make_button(screen, 88, ACTION_Y, 104, ACTION_H, "CALL", COL_GREEN,
                             0x1E8449, COL_TEXT, &lv_font_montserrat_20, call_cb, NULL);
    s_answer_btn = make_button(screen, 8, ACTION_Y, 92, ACTION_H, "ANSWER", COL_GREEN,
                               0x1E8449, COL_TEXT, &lv_font_montserrat_16, answer_cb, NULL);
    s_reject_btn = make_button(screen, 108, ACTION_Y, 84, ACTION_H, "REJECT", COL_RED,
                               0xB03A2E, COL_TEXT, &lv_font_montserrat_16, reject_cb, NULL);
    s_hangup_btn = make_button(screen, 8, ACTION_Y, 184, ACTION_H, "HANGUP", COL_RED,
                               0xB03A2E, COL_TEXT, &lv_font_montserrat_20, hangup_cb, NULL);
    make_button(screen, 200, ACTION_Y, 64, ACTION_H, "MENU", COL_CARD_HI, COL_PRIMARY, COL_TEXT,
                &lv_font_montserrat_14, menu_cb, NULL);

    /* Only the dial controls are shown until the first tick picks the row. */
    set_shown(s_answer_btn, false);
    set_shown(s_reject_btn, false);
    set_shown(s_hangup_btn, false);

    ui_settings_create(screen);

    lv_timer_create(ui_tick_cb, 500, NULL);

    ESP_LOGI(TAG, "Home screen ready (272x480 portrait)");
    return ESP_OK;
}
