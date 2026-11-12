/*
 * Home screen for the 480x272 touch panel.
 *
 * Phase 2 scope: prove display, touch and LVGL with a real dialpad. The CALL
 * button hands the number to phone_control (the call orchestrator), never to
 * the SIP stack, and reports the real error when a call cannot start.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "config_manager.h"
#include "phone_control.h"
#include "sip_client.h"
#include "ui.h"
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

static lv_obj_t *s_ext_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_sip_label;
static lv_obj_t *s_number_label;
static lv_obj_t *s_last_key;

static char s_number[DIAL_MAX_LEN + 1];
static char s_notice[48];
static uint32_t s_notice_until;

static void style_button(lv_obj_t *btn, uint32_t color, uint32_t pressed_color,
                         uint32_t text_color, const lv_font_t *font)
{
    lv_obj_remove_style_all(btn);
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
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    style_button(btn, color, pressed, text_color, font);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
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
    if (len + strlen(key) < sizeof(s_number)) {
        memcpy(&s_number[len], key, strlen(key) + 1);
        s_last_key = lv_event_get_target(event);
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

static void placeholder_cb(lv_event_t *event)
{
    const char *name = lv_event_get_user_data(event);
    show_notice(name, COL_WARN);
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
        lv_label_set_text_fmt(s_ext_label, "Ext: %s", cfg->sip.extension[0] ? cfg->sip.extension : "--");
    }

    if (wifi_manager_is_connected()) {
        lv_label_set_text_fmt(s_wifi_label, "WiFi %d dBm", wifi_manager_get_rssi());
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

    sip_call_state_t call_state = sip_client_get_call_state();
    if (call_state != SIP_CALL_IDLE) {
        lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text_fmt(s_number_label, "%s  %s", call_state_text(call_state), s_number);
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

    /* Header */
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 480, 30);
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
    lv_obj_set_pos(name, 8, 7);

    s_ext_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_ext_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ext_label, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_ext_label, "Ext: --");
    lv_obj_set_pos(s_ext_label, 168, 7);

    s_wifi_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_wifi_label, "WiFi --");
    lv_obj_set_pos(s_wifi_label, 272, 7);

    s_sip_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_sip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sip_label, lv_color_hex(COL_WARN), 0);
    lv_label_set_text(s_sip_label, "SIP OFFLINE");
    lv_obj_set_pos(s_sip_label, 386, 7);

    /* Dialled number / call state */
    s_number_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_number_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_number_label, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_align(s_number_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_number_label, 0, 32);
    lv_obj_set_size(s_number_label, 480, 30);
    lv_label_set_text(s_number_label, "Enter number");

    /* Dialpad */
    static const char *const keys[4][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {"*", "0", "#"},
    };
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 3; col++) {
            int x = 8 + col * 157;
            int y = 66 + row * 44;
            make_button(screen, x, y, 150, 38, keys[row][col], COL_CARD, COL_CARD_HI, COL_TEXT,
                        &lv_font_montserrat_20, dial_key_cb, (void *)keys[row][col]);
        }
    }

    /* Bottom bar */
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 238);
    lv_obj_set_size(bar, 480, 34);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_button(bar, 6, 2, 60, 30, "Contacts", COL_CARD, COL_CARD_HI, COL_TEXT,
                &lv_font_montserrat_14, placeholder_cb, "Contacts: phase 5");
    make_button(bar, 70, 2, 60, 30, "History", COL_CARD, COL_CARD_HI, COL_TEXT,
                &lv_font_montserrat_14, placeholder_cb, "History: phase 5");
    make_button(bar, 134, 2, 60, 30, "Settings", COL_CARD, COL_CARD_HI, COL_TEXT,
                &lv_font_montserrat_14, placeholder_cb, "Settings: phase 5");
    make_button(bar, 198, 2, 60, 30, "Status", COL_CARD, COL_CARD_HI, COL_TEXT,
                &lv_font_montserrat_14, placeholder_cb, "Status: phase 5");

    lv_obj_t *call_btn = make_button(bar, 268, 2, 96, 30, "CALL", COL_GREEN, 0x1E8449, COL_TEXT,
                                     &lv_font_montserrat_16, call_cb, NULL);
    (void)call_btn;
    lv_obj_t *del_btn = make_button(bar, 372, 2, 100, 30, "DELETE", COL_RED, 0xB03A2E, COL_TEXT,
                                    &lv_font_montserrat_16, delete_cb, NULL);
    lv_obj_add_event_cb(del_btn, delete_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_timer_create(ui_tick_cb, 500, NULL);

    ESP_LOGI(TAG, "Home screen ready (%dx%d)", 480, 272);
    return ESP_OK;
}
