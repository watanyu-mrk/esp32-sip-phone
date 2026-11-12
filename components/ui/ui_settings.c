/*
 * Settings: a hub with four pages - display/audio, SIP account, Wi-Fi and
 * live status.
 *
 * Text entry does not use lv_keyboard: on this 272x480 panel the LVGL keyboard
 * widget never became visible, so every field is a button that opens a
 * full-screen editor with a keypad built from ordinary lv_button objects -
 * the same primitives as the dialpad, which are proven to render and accept
 * touches on this board.
 *
 * All writes go through config_manager_set(), the same path the web UI uses.
 * Wi-Fi is applied immediately; SIP still needs a reboot because
 * sip_client_init() reads the account only once, so the page offers a Reboot
 * button instead of claiming the change is live. Passwords are never logged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define SCR_W 272
#define SCR_H 480

/* Editable fields, in the order the pages present them. */
typedef enum {
    F_SIP_SERVER = 0,
    F_SIP_PORT,
    F_SIP_EXT,
    F_SIP_USER,
    F_SIP_PASS,
    F_WIFI_SSID,
    F_WIFI_PASS,
    F_COUNT
} field_id_t;

static const char *const s_field_name[F_COUNT] = {
    "Server", "Port", "Extension", "Username", "Password", "SSID", "Password",
};
static const bool s_field_secret[F_COUNT] = {false, false, false, false, true, false, true};

static lv_obj_t *s_home;
static lv_obj_t *s_hub;
static lv_obj_t *s_page_display;
static lv_obj_t *s_page_sip;
static lv_obj_t *s_page_wifi;
static lv_obj_t *s_page_status;
static lv_obj_t *s_page_edit;

static lv_obj_t *s_field_btn[F_COUNT];
static char s_field[F_COUNT][CFG_SECRET_LEN];

static lv_obj_t *s_edit_title;
static lv_obj_t *s_edit_value;
static lv_obj_t *s_key_btn[4][10];
static char s_key_ch[4][10][2];
static char s_edit_buf[CFG_SECRET_LEN];
static field_id_t s_edit_id;
static bool s_shift;

static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_gain_label;
static lv_obj_t *s_info_label;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_sip_status;
static lv_obj_t *s_scan_list;
static char s_scan_ssid[WIFI_MANAGER_SCAN_MAX][33];

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

/* ------------------------------------------------------------ editable box */

static void field_refresh(field_id_t id)
{
    lv_obj_t *btn = s_field_btn[id];
    if (btn == NULL) {
        return;
    }
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label == NULL) {
        return;
    }

    const char *value = s_field[id];
    if (value[0] == '\0') {
        lv_label_set_text(label, "(tap to edit)");
        lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), 0);
    } else if (s_field_secret[id]) {
        lv_label_set_text(label, "********");
        lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
    } else {
        lv_label_set_text(label, value);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
    }
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
}

static void edit_refresh(void)
{
    if (s_edit_value == NULL) {
        return;
    }
    if (s_field_secret[s_edit_id]) {
        char stars[CFG_SECRET_LEN + 1];
        size_t len = strlen(s_edit_buf);
        if (len >= sizeof(stars)) {
            len = sizeof(stars) - 1;
        }
        memset(stars, '*', len);
        stars[len] = '\0';
        lv_label_set_text(s_edit_value, len ? stars : "(empty)");
    } else {
        lv_label_set_text(s_edit_value, s_edit_buf[0] ? s_edit_buf : "(empty)");
    }
    lv_obj_set_style_text_color(s_edit_value,
                                lv_color_hex(s_edit_buf[0] ? COL_TEXT : COL_DIM), 0);
}

static lv_obj_t *field_page(field_id_t id)
{
    return (id == F_WIFI_SSID || id == F_WIFI_PASS) ? s_page_wifi : s_page_sip;
}

static void keys_refresh(void);

static void open_editor(field_id_t id)
{
    s_edit_id = id;
    snprintf(s_edit_buf, sizeof(s_edit_buf), "%s", s_field[id]);
    s_shift = false;
    keys_refresh();
    lv_label_set_text_fmt(s_edit_title, "Edit %s", s_field_name[id]);
    edit_refresh();
    lv_screen_load(s_page_edit);
}

static void field_open_cb(lv_event_t *event)
{
    open_editor((field_id_t)(intptr_t)lv_event_get_user_data(event));
}

static lv_obj_t *make_field(lv_obj_t *page, int x, int y, int w, int h, field_id_t id)
{
    lv_obj_t *btn = lv_button_create(page);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_set_style_radius(btn, 6, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(label, "");
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

    s_field_btn[id] = btn;
    field_refresh(id);
    lv_obj_add_event_cb(btn, field_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return btn;
}

/* -------------------------------------------------------- editor keyboard */

static const char *const s_rows_lower[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl@",
    "zxcvbnm._-",
};
static const char *const s_rows_upper[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL@",
    "ZXCVBNM._-",
};

static void keys_refresh(void)
{
    const char *const *rows = s_shift ? s_rows_upper : s_rows_lower;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            char ch = rows[r][c];
            s_key_ch[r][c][0] = ch;
            s_key_ch[r][c][1] = '\0';
            if (s_key_btn[r][c] != NULL) {
                lv_obj_t *label = lv_obj_get_child(s_key_btn[r][c], 0);
                if (label != NULL) {
                    char text[2] = {ch, '\0'};
                    lv_label_set_text(label, text);
                }
            }
        }
    }
}

static void key_cb(lv_event_t *event)
{
    const char *key = lv_event_get_user_data(event);
    size_t len = strlen(s_edit_buf);

    if (strcmp(key, "SH") == 0) {
        s_shift = !s_shift;
        keys_refresh();
        return;
    }
    if (strcmp(key, "SP") == 0) {
        if (len + 1 < sizeof(s_edit_buf)) {
            s_edit_buf[len] = ' ';
            s_edit_buf[len + 1] = '\0';
        }
        edit_refresh();
        return;
    }
    if (strcmp(key, "BS") == 0) {
        if (len > 0) {
            s_edit_buf[len - 1] = '\0';
        }
        edit_refresh();
        return;
    }

    /* The port field only ever holds digits. */
    if (s_edit_id == F_SIP_PORT && (key[0] < '0' || key[0] > '9')) {
        return;
    }
    if (len + 1 < sizeof(s_edit_buf)) {
        s_edit_buf[len] = key[0];
        s_edit_buf[len + 1] = '\0';
    }
    edit_refresh();
}

static void edit_cancel_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(field_page(s_edit_id));
}

static void edit_ok_cb(lv_event_t *event)
{
    (void)event;
    snprintf(s_field[s_edit_id], sizeof(s_field[s_edit_id]), "%s", s_edit_buf);
    field_refresh(s_edit_id);
    ESP_LOGI(TAG, "Field '%s' updated", s_field_name[s_edit_id]);
    lv_screen_load(field_page(s_edit_id));
}

static void build_editor_page(void)
{
    s_page_edit = make_page();

    lv_obj_t *header = lv_obj_create(s_page_edit);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, SCR_W, 30);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_edit_title = make_label(header, 8, 6, &lv_font_montserrat_16, COL_TEXT, "Edit");
    make_button(header, 190, 4, 74, 22, "Cancel", COL_CARD, COL_CARD_HI,
                &lv_font_montserrat_14, edit_cancel_cb, NULL);

    lv_obj_t *box = make_card(s_page_edit, 8, 36, 256, 40);
    s_edit_value = make_label(box, 10, 10, &lv_font_montserrat_16, COL_TEXT, "");
    lv_obj_set_size(s_edit_value, 236, 22);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int x = 7 + c * 26;
            int y = 86 + r * 42;
            s_key_btn[r][c] = make_button(s_page_edit, x, y, 24, 40, "", COL_CARD, COL_PRIMARY,
                                          &lv_font_montserrat_14, key_cb, s_key_ch[r][c]);
        }
    }
    keys_refresh();

    make_button(s_page_edit, 8, 260, 72, 38, "abc", COL_CARD_HI, COL_PRIMARY,
                &lv_font_montserrat_14, key_cb, "SH");
    make_button(s_page_edit, 86, 260, 100, 38, "SPACE", COL_CARD, COL_PRIMARY,
                &lv_font_montserrat_14, key_cb, "SP");
    make_button(s_page_edit, 192, 260, 72, 38, "DEL", COL_CARD, COL_PRIMARY,
                &lv_font_montserrat_14, key_cb, "BS");

    make_button(s_page_edit, 8, 310, 256, 46, "OK", COL_GREEN, 0x1E8449,
                &lv_font_montserrat_20, edit_ok_cb, NULL);

    make_label(s_page_edit, 8, 366, &lv_font_montserrat_14, COL_DIM,
               "Cancel keeps the previous value.");
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
    const char *server = s_field[F_SIP_SERVER];
    const char *ext = s_field[F_SIP_EXT];

    if (server[0] == '\0' || ext[0] == '\0') {
        lv_obj_set_style_text_color(s_sip_status, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_sip_status, "Server and extension are required");
        return;
    }

    long port = strtol(s_field[F_SIP_PORT], NULL, 10);
    if (port < 1 || port > 65535) {
        port = 5060;
    }

    phone_config_t cfg = *config_manager_get();
    /* Bound the source to the destination capacity: the editor buffers are
     * larger than several SIP fields, and -Wformat-truncation is fatal here. */
    snprintf(cfg.sip.server, sizeof(cfg.sip.server), "%.*s",
             (int)sizeof(cfg.sip.server) - 1, server);
    cfg.sip.port = (uint16_t)port;
    snprintf(cfg.sip.extension, sizeof(cfg.sip.extension), "%.*s",
             (int)sizeof(cfg.sip.extension) - 1, ext);
    const char *user = (s_field[F_SIP_USER][0] != '\0') ? s_field[F_SIP_USER] : ext;
    snprintf(cfg.sip.username, sizeof(cfg.sip.username), "%.*s",
             (int)sizeof(cfg.sip.username) - 1, user);
    if (s_field[F_SIP_PASS][0] != '\0') {
        snprintf(cfg.sip.password, sizeof(cfg.sip.password), "%s", s_field[F_SIP_PASS]);
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
    const char *ssid = s_field[F_WIFI_SSID];

    if (ssid[0] == '\0') {
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_wifi_status, "SSID is required");
        return;
    }

    phone_config_t cfg = *config_manager_get();
    snprintf(cfg.wifi.ssid, sizeof(cfg.wifi.ssid), "%.*s", (int)sizeof(cfg.wifi.ssid) - 1,
             ssid);
    if (s_field[F_WIFI_PASS][0] != '\0') {
        snprintf(cfg.wifi.password, sizeof(cfg.wifi.password), "%s", s_field[F_WIFI_PASS]);
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

static void wifi_pick_cb(lv_event_t *event)
{
    const char *ssid = lv_event_get_user_data(event);
    if (ssid == NULL) {
        return;
    }
    snprintf(s_field[F_WIFI_SSID], sizeof(s_field[F_WIFI_SSID]), "%s", ssid);
    field_refresh(F_WIFI_SSID);
    lv_label_set_text_fmt(s_wifi_status, "Selected %s", ssid);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_DIM), 0);
}

static void wifi_scan_cb(lv_event_t *event)
{
    (void)event;

    /* The scan blocks for a few seconds; paint the notice first. */
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_WARN), 0);
    lv_label_set_text(s_wifi_status, "Scanning...");
    lv_refr_now(NULL);

    wifi_scan_ap_t aps[WIFI_MANAGER_SCAN_MAX];
    int found = wifi_manager_scan(aps, WIFI_MANAGER_SCAN_MAX);

    lv_obj_clean(s_scan_list);
    if (found <= 0) {
        lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_WARN), 0);
        lv_label_set_text(s_wifi_status, (found == 0) ? "No networks found" : "Scan failed");
        return;
    }

    for (int i = 0; i < found; i++) {
        snprintf(s_scan_ssid[i], sizeof(s_scan_ssid[i]), "%s", aps[i].ssid);

        lv_obj_t *row = lv_button_create(s_scan_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 240, 34);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD_HI), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 6, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text_fmt(label, "%s  %d dBm%s", aps[i].ssid, (int)aps[i].rssi,
                              aps[i].secure ? " *" : "");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_add_event_cb(row, wifi_pick_cb, LV_EVENT_CLICKED, s_scan_ssid[i]);
    }

    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COL_DIM), 0);
    lv_label_set_text_fmt(s_wifi_status, "%d networks - tap to pick", found);
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
    s_page_sip = make_page();
    add_header(s_page_sip, "SIP account", back_hub_cb);

    make_label(s_page_sip, 8, 32, &lv_font_montserrat_14, COL_DIM, "Server");
    make_field(s_page_sip, 8, 48, 256, 30, F_SIP_SERVER);

    make_label(s_page_sip, 8, 84, &lv_font_montserrat_14, COL_DIM, "Port");
    make_field(s_page_sip, 8, 100, 110, 30, F_SIP_PORT);
    make_label(s_page_sip, 134, 84, &lv_font_montserrat_14, COL_DIM, "Extension");
    make_field(s_page_sip, 134, 100, 130, 30, F_SIP_EXT);

    make_label(s_page_sip, 8, 136, &lv_font_montserrat_14, COL_DIM, "Username");
    make_field(s_page_sip, 8, 152, 256, 30, F_SIP_USER);

    make_label(s_page_sip, 8, 188, &lv_font_montserrat_14, COL_DIM, "Password (blank = keep)");
    make_field(s_page_sip, 8, 204, 256, 30, F_SIP_PASS);

    make_button(s_page_sip, 8, 244, 120, 34, "Save", COL_GREEN, 0x1E8449,
                &lv_font_montserrat_16, sip_save_cb, NULL);
    make_button(s_page_sip, 136, 244, 120, 34, "Reboot", COL_RED, 0xB03A2E,
                &lv_font_montserrat_16, reboot_cb, NULL);

    s_sip_status = make_label(s_page_sip, 8, 288, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_sip_status, 256, 60);
    lv_label_set_text(s_sip_status, "Changes apply after reboot.");
}

static void build_wifi_page(void)
{
    s_page_wifi = make_page();
    add_header(s_page_wifi, "Wi-Fi", back_hub_cb);

    make_label(s_page_wifi, 8, 32, &lv_font_montserrat_14, COL_DIM, "SSID");
    make_field(s_page_wifi, 8, 48, 190, 30, F_WIFI_SSID);
    make_button(s_page_wifi, 204, 48, 60, 30, "Scan", COL_PRIMARY, 0x1F4FB0,
                &lv_font_montserrat_14, wifi_scan_cb, NULL);

    make_label(s_page_wifi, 8, 84, &lv_font_montserrat_14, COL_DIM, "Password (blank = keep)");
    make_field(s_page_wifi, 8, 100, 256, 30, F_WIFI_PASS);

    make_button(s_page_wifi, 8, 138, 130, 34, "Connect", COL_GREEN, 0x1E8449,
                &lv_font_montserrat_16, wifi_connect_cb, NULL);

    s_wifi_status = make_label(s_page_wifi, 8, 180, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_wifi_status, 256, 30);

    s_scan_list = lv_obj_create(s_page_wifi);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_pos(s_scan_list, 8, 214);
    lv_obj_set_size(s_scan_list, 256, 176);
    lv_obj_set_style_pad_all(s_scan_list, 0, 0);
    lv_obj_set_style_pad_row(s_scan_list, 4, 0);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_scan_list, LV_DIR_VER);

    make_label(s_page_wifi, 8, 398, &lv_font_montserrat_14, COL_DIM, "* = secured network");
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

static void load_fields(void)
{
    const phone_config_t *cfg = config_manager_get();
    if (cfg == NULL) {
        return;
    }
    snprintf(s_field[F_SIP_SERVER], sizeof(s_field[F_SIP_SERVER]), "%s", cfg->sip.server);
    snprintf(s_field[F_SIP_PORT], sizeof(s_field[F_SIP_PORT]), "%u", (unsigned)cfg->sip.port);
    snprintf(s_field[F_SIP_EXT], sizeof(s_field[F_SIP_EXT]), "%s", cfg->sip.extension);
    snprintf(s_field[F_SIP_USER], sizeof(s_field[F_SIP_USER]), "%s", cfg->sip.username);
    snprintf(s_field[F_WIFI_SSID], sizeof(s_field[F_WIFI_SSID]), "%s", cfg->wifi.ssid);
    /* Password boxes start empty: blank means "keep the stored secret". */
    s_field[F_SIP_PASS][0] = '\0';
    s_field[F_WIFI_PASS][0] = '\0';
}

void ui_settings_create(lv_obj_t *home_screen)
{
    s_home = home_screen;
    load_fields();

    build_hub();
    build_display_page();
    build_sip_page();
    build_wifi_page();
    build_status_page();
    build_editor_page();

    info_tick_cb(NULL);
    lv_timer_create(info_tick_cb, 1000, NULL);

    ESP_LOGI(TAG, "Settings hub ready (5 pages incl. editor)");
}

void ui_settings_open(void)
{
    if (s_hub == NULL) {
        return;
    }
    lv_screen_load(s_hub);
    ESP_LOGI(TAG, "Settings opened");
}
