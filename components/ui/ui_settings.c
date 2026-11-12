/*
 * Settings: a hub with four pages - display/audio, SIP account, Wi-Fi and
 * live status.
 *
 * All writes go through config_manager_set(), the same path the web UI uses,
 * so nothing here bypasses validation. Two behaviours match the web UI:
 *   - Wi-Fi credentials are applied immediately by reconnecting the station.
 *   - SIP changes are saved but need a reboot, because sip_client_init() reads
 *     the account once and returns early after that; the page offers a Reboot
 *     button instead of pretending the change is live.
 *
 * Passwords are never logged.
 */
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "audio_driver.h"
#include "config_manager.h"
#include "display_driver.h"
#include "sip_client.h"
#include "ui_settings.h"
#include "wifi_manager.h"

static const char *TAG = "UI.SET";

#define COL_BG       0x101A2B
#define COL_HEADER   0x0B1220
#define COL_CARD     0x1B2A44
#define COL_CARD_HI  0x26395A
#define COL_TEXT     0xE8EEF7
#define COL_DIM      0x93A4BF
#define COL_PRIMARY  0x2F6FED
#define COL_GREEN    0x27AE60
#define COL_WARN     0xF39C12
#define COL_RED      0xE74C3C

/* Screen is 272x480; the keyboard covers the bottom 208 px while editing. */
#define SCR_W 272
#define SCR_H 480
#define KB_H  208

static lv_obj_t *s_home;     /* dialpad */
static lv_obj_t *s_hub;      /* this settings menu */
static lv_obj_t *s_page_display;
static lv_obj_t *s_page_sip;
static lv_obj_t *s_page_wifi;
static lv_obj_t *s_page_status;

static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_gain_label;
static lv_obj_t *s_info_label;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_sip_status;

static lv_obj_t *s_sip_server;
static lv_obj_t *s_sip_port;
static lv_obj_t *s_sip_ext;
static lv_obj_t *s_sip_user;
static lv_obj_t *s_sip_pass;
static lv_obj_t *s_wifi_ssid;
static lv_obj_t *s_wifi_pass;

static int8_t s_gain_db;

/* ---------------------------------------------------------------- helpers */

static lv_obj_t *make_page(void)
{
    lv_obj_t *page = lv_obj_create(NULL);
    lv_obj_remove_style_all(page);
    lv_obj_set_style_bg_color(page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color,
                            const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                             uint32_t color, uint32_t pressed, const lv_font_t *font,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    /* Remove theme styles before setting geometry; doing it afterwards would
     * delete the position and size that were just stored. */
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(pressed), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t *make_slider(lv_obj_t *parent, int x, int y, int w, int min, int max, int value)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, w, 12);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 6, LV_PART_KNOB);
    return slider;
}

static lv_obj_t *make_textarea(lv_obj_t *page, int x, int y, int w, int h, const char *text,
                               uint32_t max_len, bool password, lv_event_cb_t focus_cb,
                               lv_obj_t *keyboard)
{
    lv_obj_t *ta = lv_textarea_create(page);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, h);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, (text != NULL) ? text : "");
    if (password) {
        lv_textarea_set_password_mode(ta, true);
    }

    lv_obj_set_style_bg_color(ta, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_all(ta, 6, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);

    /* Registered by the page once its keyboard exists; a focus callback with
     * a NULL keyboard would crash on the first tap. */
    if (keyboard != NULL) {
        lv_obj_add_event_cb(ta, focus_cb, LV_EVENT_CLICKED, keyboard);
    }
    return ta;
}

static void keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *kb = lv_event_get_target(event);
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_keyboard(lv_obj_t *page)
{
    lv_obj_t *kb = lv_keyboard_create(page);
    lv_obj_set_pos(kb, 0, SCR_H - KB_H);
    lv_obj_set_size(kb, SCR_W, KB_H);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(kb, keyboard_event_cb, LV_EVENT_ALL, NULL);
    return kb;
}

static void ta_text_cb(lv_event_t *event)
{
    lv_obj_t *kb = lv_event_get_user_data(event);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, lv_event_get_target(event));
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void ta_number_cb(lv_event_t *event)
{
    lv_obj_t *kb = lv_event_get_user_data(event);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, lv_event_get_target(event));
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------- navigation */

static void back_home_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_home);
}

static void back_hub_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_hub);
}

static void open_display_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_page_display);
}

static void open_sip_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_page_sip);
}

static void open_wifi_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_page_wifi);
}

static void open_status_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_page_status);
}

/* -------------------------------------------------------- display / audio */

static void brightness_changed_cb(lv_event_t *event)
{
    (void)event;
    int value = lv_slider_get_value(s_brightness_slider);
    display_driver_set_backlight((uint8_t)value);
    lv_label_set_text_fmt(s_brightness_label, "Brightness %d%%", value);
}

static void volume_changed_cb(lv_event_t *event)
{
    (void)event;
    int value = lv_slider_get_value(s_volume_slider);
    audio_set_volume((uint8_t)value);
    lv_label_set_text_fmt(s_volume_label, "Volume %d%%", value);
}

static void volume_released_cb(lv_event_t *event)
{
    (void)event;
    const phone_config_t *cur = config_manager_get();
    if (cur == NULL) {
        return;
    }
    phone_config_t cfg = *cur;
    cfg.audio.speaker_volume = (uint8_t)lv_slider_get_value(s_volume_slider);
    if (config_manager_set(&cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Volume saved: %u%%", cfg.audio.speaker_volume);
    } else {
        ESP_LOGW(TAG, "Volume save failed");
    }
}

static void gain_apply(int8_t db)
{
    if (db < 0) {
        db = 0;
    } else if (db > 12) {
        db = 12;
    }
    if (audio_set_mic_gain_db(db) != ESP_OK) {
        return;
    }
    s_gain_db = db;
    lv_label_set_text_fmt(s_gain_label, "Mic gain %+d dB", db);

    const phone_config_t *cur = config_manager_get();
    if (cur) {
        phone_config_t cfg = *cur;
        cfg.audio.mic_gain_db = db;
        if (config_manager_set(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "Mic gain save failed");
        }
    }
}

static void gain_down_cb(lv_event_t *event)
{
    (void)event;
    gain_apply((int8_t)(s_gain_db - 3));
}

static void gain_up_cb(lv_event_t *event)
{
    (void)event;
    gain_apply((int8_t)(s_gain_db + 3));
}

/* ------------------------------------------------------------------- SIP */

static void sip_save_cb(lv_event_t *event)
{
    (void)event;
    const char *server = lv_textarea_get_text(s_sip_server);
    const char *port_text = lv_textarea_get_text(s_sip_port);
    const char *ext = lv_textarea_get_text(s_sip_ext);
    const char *user = lv_textarea_get_text(s_sip_user);
    const char *pass = lv_textarea_get_text(s_sip_pass);

    if (server[0] == '\0' || ext[0] == '\0') {
        lv_obj_set_style_text_color(s_sip_status, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_sip_status, "Server and extension are required");
        return;
    }

    long port = strtol(port_text, NULL, 10);
    if (port < 1 || port > 65535) {
        port = 5060;
    }

    phone_config_t cfg = *config_manager_get();
    snprintf(cfg.sip.server, sizeof(cfg.sip.server), "%s", server);
    cfg.sip.port = (uint16_t)port;
    snprintf(cfg.sip.extension, sizeof(cfg.sip.extension), "%s", ext);
    snprintf(cfg.sip.username, sizeof(cfg.sip.username), "%s", (user[0] != '\0') ? user : ext);
    if (pass[0] != '\0') {
        snprintf(cfg.sip.password, sizeof(cfg.sip.password), "%s", pass);
    }

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        lv_obj_set_style_text_color(s_sip_status, lv_color_hex(COL_RED), 0);
        lv_label_set_text(s_sip_status, "Save failed");
        ESP_LOGE(TAG, "SIP save failed: %s", esp_err_to_name(err));
        return;
    }

    lv_obj_set_style_text_color(s_sip_status, lv_color_hex(COL_GREEN), 0);
    lv_label_set_text(s_sip_status, "Saved. Reboot to register.");
    ESP_LOGI(TAG, "SIP account saved (server '%s', extension '%s')", cfg.sip.server,
             cfg.sip.extension);
}

static void reboot_cb(lv_event_t *event)
{
    (void)event;
    ESP_LOGW(TAG, "Reboot requested from the settings screen");
    esp_restart();
}

/* ------------------------------------------------------------------ Wi-Fi */

static void wifi_connect_cb(lv_event_t *event)
{
    (void)event;
    const char *ssid = lv_textarea_get_text(s_wifi_ssid);
    const char *pass = lv_textarea_get_text(s_wifi_pass);

    if (ssid[0] == '\0') {
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_wifi_status, "SSID is required");
        return;
    }

    phone_config_t cfg = *config_manager_get();
    snprintf(cfg.wifi.ssid, sizeof(cfg.wifi.ssid), "%s", ssid);
    if (pass[0] != '\0') {
        snprintf(cfg.wifi.password, sizeof(cfg.wifi.password), "%s", pass);
    }

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_RED), 0);
        lv_label_set_text(s_wifi_status, "Save failed");
        ESP_LOGE(TAG, "Wi-Fi save failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_manager_disconnect();
    err = wifi_manager_connect();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_RED), 0);
        lv_label_set_text_fmt(s_wifi_status, "Saved, connect failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
        return;
    }

    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_WARN), 0);
    lv_label_set_text(s_wifi_status, "Saved, connecting...");
    ESP_LOGI(TAG, "Wi-Fi saved (SSID '%s'), reconnecting", cfg.wifi.ssid);
}

/* ---------------------------------------------------------------- status */

static const char *reg_state_text(void)
{
    switch (sip_client_get_reg_state()) {
    case SIP_REG_STATE_REGISTERED: return "registered";
    case SIP_REG_STATE_REGISTERING: return "registering";
    case SIP_REG_STATE_FAILED: return "failed";
    default: return "offline";
    }
}

static void info_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const phone_config_t *cfg = config_manager_get();

    char ip[16] = {0};
    if (wifi_manager_get_ip(ip, sizeof(ip)) != ESP_OK) {
        snprintf(ip, sizeof(ip), "%s", "--");
    }

    lv_label_set_text_fmt(s_info_label,
                          "IP: %s\n"
                          "WiFi: %s (%d dBm)\n"
                          "SIP: %s\n"
                          "Server: %s:%u\n"
                          "Ext: %s\n"
                          "Heap: %u KB",
                          ip, wifi_manager_is_connected() ? "connected" : "down",
                          (int)wifi_manager_get_rssi(), reg_state_text(),
                          cfg ? cfg->sip.server : "--",
                          (unsigned)(cfg ? cfg->sip.port : 0),
                          cfg ? cfg->sip.extension : "--",
                          (unsigned)(esp_get_free_heap_size() / 1024));

    if (s_wifi_status != NULL) {
        const char *state = wifi_manager_is_connected() ? "connected" : "not connected";
        lv_label_set_text_fmt(s_wifi_status, "WiFi %s\n%s", state, ip);
        lv_obj_set_style_text_color(
            s_wifi_status, lv_color_hex(wifi_manager_is_connected() ? COL_GREEN : COL_DIM), 0);
    }
}

/* --------------------------------------------------------------- screens */

static lv_obj_t *add_header(lv_obj_t *page, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, SCR_W, 30);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    make_label(header, 8, 6, &lv_font_montserrat_16, COL_TEXT, title);
    make_button(header, 198, 4, 66, 22, "Back", COL_CARD, COL_CARD_HI, &lv_font_montserrat_14,
                back_cb, NULL);
    return header;
}

static void build_display_page(void)
{
    s_page_display = make_page();
    add_header(s_page_display, "Display & sound", back_hub_cb);

    lv_obj_t *display_card = make_card(s_page_display, 8, 38, 256, 62);
    uint8_t brightness = display_driver_get_backlight();
    if (brightness < 10) {
        brightness = 10;
    }
    s_brightness_label = make_label(display_card, 10, 6, &lv_font_montserrat_14, COL_TEXT, "");
    lv_label_set_text_fmt(s_brightness_label, "Brightness %u%%", brightness);
    s_brightness_slider = make_slider(display_card, 10, 34, 236, 10, 100, brightness);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    const phone_config_t *cfg = config_manager_get();
    lv_obj_t *audio_card = make_card(s_page_display, 8, 108, 256, 112);
    uint8_t volume = cfg ? cfg->audio.speaker_volume : AUDIO_DEFAULT_VOLUME;
    if (volume > 100) {
        volume = 100;
    }
    s_volume_label = make_label(audio_card, 10, 6, &lv_font_montserrat_14, COL_TEXT, "");
    lv_label_set_text_fmt(s_volume_label, "Volume %u%%", volume);
    s_volume_slider = make_slider(audio_card, 10, 32, 236, 0, 100, volume);
    lv_obj_add_event_cb(s_volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, volume_released_cb, LV_EVENT_RELEASED, NULL);

    s_gain_db = cfg ? cfg->audio.mic_gain_db : AUDIO_DEFAULT_MIC_GAIN_DB;
    s_gain_label = make_label(audio_card, 10, 58, &lv_font_montserrat_14, COL_TEXT, "");
    lv_label_set_text_fmt(s_gain_label, "Mic gain %+d dB", s_gain_db);
    make_button(audio_card, 10, 80, 60, 24, "-3", COL_CARD_HI, COL_PRIMARY,
                &lv_font_montserrat_14, gain_down_cb, NULL);
    make_button(audio_card, 186, 80, 60, 24, "+3", COL_CARD_HI, COL_PRIMARY,
                &lv_font_montserrat_14, gain_up_cb, NULL);

    make_label(s_page_display, 8, 236, &lv_font_montserrat_14, COL_DIM,
               "Volume and gain are saved to NVS.");
}

static void build_sip_page(void)
{
    const phone_config_t *cfg = config_manager_get();
    s_page_sip = make_page();
    add_header(s_page_sip, "SIP account", back_hub_cb);

    make_label(s_page_sip, 8, 32, &lv_font_montserrat_14, COL_DIM, "Server");
    s_sip_server = make_textarea(s_page_sip, 8, 48, 256, 30, cfg ? cfg->sip.server : "",
                                 CFG_SERVER_LEN - 1, false, ta_text_cb, NULL);

    make_label(s_page_sip, 8, 84, &lv_font_montserrat_14, COL_DIM, "Port");
    char port_text[8] = {0};
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)(cfg ? cfg->sip.port : 5060));
    s_sip_port = make_textarea(s_page_sip, 8, 100, 110, 30, port_text, 5, false, ta_number_cb, NULL);

    make_label(s_page_sip, 134, 84, &lv_font_montserrat_14, COL_DIM, "Extension");
    s_sip_ext = make_textarea(s_page_sip, 134, 100, 130, 30, cfg ? cfg->sip.extension : "",
                              CFG_EXT_LEN - 1, false, ta_number_cb, NULL);

    make_label(s_page_sip, 8, 136, &lv_font_montserrat_14, COL_DIM, "Username");
    s_sip_user = make_textarea(s_page_sip, 8, 152, 256, 30, cfg ? cfg->sip.username : "",
                               CFG_ID_LEN - 1, false, ta_text_cb, NULL);

    make_label(s_page_sip, 8, 188, &lv_font_montserrat_14, COL_DIM, "Password (blank = keep)");
    s_sip_pass = make_textarea(s_page_sip, 8, 204, 256, 30, "", CFG_SECRET_LEN - 1, true,
                               ta_text_cb, NULL);

    make_button(s_page_sip, 8, 244, 120, 34, "Save", COL_GREEN, 0x1E8449,
                &lv_font_montserrat_16, sip_save_cb, NULL);
    make_button(s_page_sip, 136, 244, 120, 34, "Reboot", COL_RED, 0xB03A2E,
                &lv_font_montserrat_16, reboot_cb, NULL);

    s_sip_status = make_label(s_page_sip, 8, 288, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_sip_status, 256, 60);
    lv_label_set_text(s_sip_status, "Changes apply after reboot.");

    /* One keyboard per text-entry page, hidden until a field is tapped. */
    lv_obj_t *kb = make_keyboard(s_page_sip);
    lv_obj_add_event_cb(s_sip_server, ta_text_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_sip_port, ta_number_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_sip_ext, ta_number_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_sip_user, ta_text_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_sip_pass, ta_text_cb, LV_EVENT_CLICKED, kb);
}

static void build_wifi_page(void)
{
    const phone_config_t *cfg = config_manager_get();
    s_page_wifi = make_page();
    add_header(s_page_wifi, "Wi-Fi", back_hub_cb);

    make_label(s_page_wifi, 8, 32, &lv_font_montserrat_14, COL_DIM, "SSID");
    s_wifi_ssid = make_textarea(s_page_wifi, 8, 48, 256, 30, cfg ? cfg->wifi.ssid : "",
                                CFG_SSID_LEN - 1, false, ta_text_cb, NULL);

    make_label(s_page_wifi, 8, 84, &lv_font_montserrat_14, COL_DIM, "Password (blank = keep)");
    s_wifi_pass = make_textarea(s_page_wifi, 8, 100, 256, 30, "", CFG_SECRET_LEN - 1, true,
                                ta_text_cb, NULL);

    make_button(s_page_wifi, 8, 140, 140, 36, "Connect", COL_GREEN, 0x1E8449,
                &lv_font_montserrat_16, wifi_connect_cb, NULL);

    s_wifi_status = make_label(s_page_wifi, 8, 186, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_wifi_status, 256, 44);

    make_label(s_page_wifi, 8, 240, &lv_font_montserrat_14, COL_DIM,
               "Saves and reconnects now.\nIf it fails, the setup AP returns.");

    lv_obj_t *kb = make_keyboard(s_page_wifi);
    lv_obj_add_event_cb(s_wifi_ssid, ta_text_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_wifi_pass, ta_text_cb, LV_EVENT_CLICKED, kb);
}

static void build_status_page(void)
{
    s_page_status = make_page();
    add_header(s_page_status, "Status", back_hub_cb);

    lv_obj_t *card = make_card(s_page_status, 8, 38, 256, 196);
    s_info_label = make_label(card, 10, 10, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_info_label, 236, 170);
}

static void build_hub(void)
{
    s_hub = make_page();
    add_header(s_hub, "Settings", back_home_cb);

    make_button(s_hub, 8, 44, 256, 46, "Display & sound", COL_CARD, COL_CARD_HI,
                &lv_font_montserrat_16, open_display_cb, NULL);
    make_button(s_hub, 8, 100, 256, 46, "SIP account", COL_CARD, COL_CARD_HI,
                &lv_font_montserrat_16, open_sip_cb, NULL);
    make_button(s_hub, 8, 156, 256, 46, "Wi-Fi", COL_CARD, COL_CARD_HI,
                &lv_font_montserrat_16, open_wifi_cb, NULL);
    make_button(s_hub, 8, 212, 256, 46, "Status", COL_CARD, COL_CARD_HI,
                &lv_font_montserrat_16, open_status_cb, NULL);

    make_label(s_hub, 8, 440, &lv_font_montserrat_14, COL_DIM, "Web UI: http://<ip>/ port 80");
}

void ui_settings_create(lv_obj_t *home_screen)
{
    s_home = home_screen;

    build_hub();
    build_display_page();
    build_sip_page();
    build_wifi_page();
    build_status_page();

    info_tick_cb(NULL);
    lv_timer_create(info_tick_cb, 1000, NULL);

    ESP_LOGI(TAG, "Settings hub ready (4 pages)");
}

void ui_settings_open(void)
{
    if (s_hub == NULL) {
        return;
    }
    lv_screen_load(s_hub);
    ESP_LOGI(TAG, "Settings opened");
}
