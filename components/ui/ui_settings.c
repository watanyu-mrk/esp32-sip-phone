/*
 * Settings screen: display, audio and live system information.
 *
 * Everything shown here is backed by a real driver or config field:
 *   - brightness applies through display_driver_set_backlight()
 *   - volume and mic gain apply through audio_driver and are saved to NVS
 *   - the info block polls wifi_manager / sip_client / config_manager
 * Credentials (Wi-Fi password, SIP account) still belong to the web UI, which
 * already validates and persists them.
 */
#include <stdio.h>

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

static lv_obj_t *s_home;
static lv_obj_t *s_settings;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_gain_label;
static lv_obj_t *s_info_label;
static int8_t s_gain_db;

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
                             lv_event_cb_t cb)
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
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
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

static void back_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(s_home);
}

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
    esp_err_t err = config_manager_set(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Volume saved: %u%%", cfg.audio.speaker_volume);
    } else {
        ESP_LOGW(TAG, "Volume save failed: %s", esp_err_to_name(err));
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
}

void ui_settings_create(lv_obj_t *home_screen)
{
    s_home = home_screen;

    s_settings = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(s_settings);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 272, 30);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    make_label(header, 8, 6, &lv_font_montserrat_16, COL_TEXT, "Settings");
    make_button(header, 198, 4, 66, 22, "Back", COL_CARD, COL_CARD_HI, &lv_font_montserrat_14,
                back_cb);

    /* Display: brightness is applied live and is not persisted, because the
     * configuration schema has no display section yet. */
    lv_obj_t *display_card = make_card(s_settings, 8, 38, 256, 62);
    uint8_t brightness = display_driver_get_backlight();
    if (brightness < 10) {
        brightness = 10;
    }
    s_brightness_label = make_label(display_card, 10, 6, &lv_font_montserrat_14, COL_TEXT, "");
    lv_label_set_text_fmt(s_brightness_label, "Brightness %u%%", brightness);
    s_brightness_slider = make_slider(display_card, 10, 34, 236, 10, 100, brightness);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Audio: volume and mic gain both persist to NVS. */
    const phone_config_t *cfg = config_manager_get();
    lv_obj_t *audio_card = make_card(s_settings, 8, 108, 256, 112);
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
                &lv_font_montserrat_14, gain_down_cb);
    make_button(audio_card, 186, 80, 60, 24, "+3", COL_CARD_HI, COL_PRIMARY,
                &lv_font_montserrat_14, gain_up_cb);

    /* Live status block. */
    lv_obj_t *info_card = make_card(s_settings, 8, 228, 256, 176);
    make_label(info_card, 10, 8, &lv_font_montserrat_16, COL_TEXT, "Status");
    s_info_label = make_label(info_card, 10, 34, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_size(s_info_label, 236, 134);

    make_label(s_settings, 8, 418, &lv_font_montserrat_14, COL_DIM,
               "Credentials: web UI on port 80");

    info_tick_cb(NULL);
    lv_timer_create(info_tick_cb, 1000, NULL);

    ESP_LOGI(TAG, "Settings screen ready");
}

void ui_settings_open(void)
{
    if (s_settings == NULL) {
        return;
    }
    info_tick_cb(NULL);
    lv_screen_load(s_settings);
    ESP_LOGI(TAG, "Settings screen opened");
}
