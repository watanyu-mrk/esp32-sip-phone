/*
 * XIAO ESP32-S3 Sense SIP IP phone - boot skeleton.
 *
 * This phase only brings the system up and proves the module wiring:
 *   - configuration manager (NVS) is fully functional
 *   - every other module is a stub that reports ESP_ERR_NOT_SUPPORTED
 *
 * No SIP, RTP or audio path exists yet. Per the project rules, none of it is
 * faked: the stubs are explicit so nothing claims to place a call.
 */
#include <stddef.h>

#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"
#include "audio_driver.h"
#include "config_manager.h"
#include "phone_control.h"
#include "rtp_client.h"
#include "sip_client.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "BOOT";

/* Longest the boot sequence waits for an IP before continuing. */
#define BOOT_WIFI_WAIT_MS 10000

static void log_banner(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "       %s", APP_NAME);
    ESP_LOGI(TAG, "========================================");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "[BOOT] Device Starting...");
    ESP_LOGI(TAG, "[BOOT] Board: %s", APP_BOARD);
    ESP_LOGI(TAG, "[BOOT] Firmware: %s", APP_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "[BOOT] Chip: %s, %d core(s), rev v%d.%d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision / 100, chip.revision % 100);

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        ESP_LOGI(TAG, "[BOOT] PSRAM: %u KB total, %u KB free",
                 (unsigned)(psram_total / 1024), (unsigned)(psram_free / 1024));
    } else {
        ESP_LOGW(TAG, "[BOOT] PSRAM: not detected - check sdkconfig/board variant");
    }

    ESP_LOGI(TAG, "[BOOT] Internal heap free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void log_init_result(const char *module, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[BOOT] %s: ready", module);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "[BOOT] %s: scaffold stub only (not implemented yet)", module);
    } else {
        ESP_LOGE(TAG, "[BOOT] %s: init failed (%s)", module, esp_err_to_name(err));
    }
}

static bool wait_for_wifi(uint32_t timeout_ms)
{
    const uint32_t step_ms = 250;

    for (uint32_t waited = 0; waited < timeout_ms; waited += step_ms) {
        if (wifi_manager_is_connected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    return wifi_manager_is_connected();
}

static void start_config_portal(void)
{
    esp_err_t err = wifi_manager_start_fallback_ap();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "[WIFI] Configuration AP: SSID '%s' (open), http://192.168.4.1",
                 WIFI_FALLBACK_AP_SSID);
    } else {
        ESP_LOGE(TAG, "[WIFI] Configuration AP failed: %s", esp_err_to_name(err));
    }
}

/* Connects using the stored configuration. If the credentials are missing or
 * the station cannot get an IP, the open configuration AP is raised instead. */
static void connect_wifi_at_boot(void)
{
    const phone_config_t *cfg = config_manager_get();

    if (cfg->wifi.ssid[0] == '\0') {
        ESP_LOGW(TAG, "[WIFI] No SSID configured, starting configuration AP");
        start_config_portal();
        return;
    }

    ESP_LOGI(TAG, "[WIFI] Connecting to WiFi...");
    esp_err_t err = wifi_manager_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Connect request failed (%s)", esp_err_to_name(err));
        start_config_portal();
        app_state_set_wifi(false, -127);
        return;
    }

    if (!wait_for_wifi(BOOT_WIFI_WAIT_MS)) {
        ESP_LOGW(TAG, "[WIFI] No IP after %d ms, starting configuration AP",
                 BOOT_WIFI_WAIT_MS);
        start_config_portal();
        app_state_set_wifi(false, -127);
        return;
    }

    char ip[16] = {0};
    esp_err_t ip_err = wifi_manager_get_ip(ip, sizeof(ip));
    int8_t rssi = wifi_manager_get_rssi();
    ESP_LOGI(TAG, "[WIFI] Connected, IP: %s, RSSI: %d dBm",
             (ip_err == ESP_OK) ? ip : "unknown", rssi);
    app_state_set_wifi(true, rssi);
}

void app_main(void)
{
    log_banner();
    app_state_init();

    esp_err_t err = config_manager_init();
    log_init_result("config_manager", err);
    if (err == ESP_OK) {
        const phone_config_t *cfg = config_manager_get();
        ESP_LOGI(TAG, "[BOOT] Device name: %s", cfg->system.device_name);
        ESP_LOGI(TAG, "[SYSTEM] Configured SIP: %s:%u, extension %s",
                 cfg->sip.server, (unsigned)cfg->sip.port, cfg->sip.extension);
    }

    esp_err_t wifi_err = wifi_manager_init();
    log_init_result("wifi_manager", wifi_err);
    if (wifi_err == ESP_OK) {
        connect_wifi_at_boot();
    }

    esp_err_t audio_err = audio_driver_init();
    log_init_result("audio_driver", audio_err);
    if (audio_err == ESP_OK) {
        /* Capture starts at boot so the serial meter proves the PDM mic works
         * before any call exists. Playback stays idle until audio arrives. */
        esp_err_t in_err = audio_input_start();
        if (in_err == ESP_OK) {
            ESP_LOGI(TAG, "[AUDIO] Microphone capture running, level meter on serial");
        } else {
            ESP_LOGE(TAG, "[AUDIO] Microphone start failed: %s", esp_err_to_name(in_err));
        }
    }
    log_init_result("sip_client", sip_client_init());
    log_init_result("rtp_audio", rtp_client_init());
    log_init_result("phone_control", phone_control_init());
    esp_err_t web_err = web_server_start();
    log_init_result("web_server", web_err);
    if (web_err == ESP_OK) {
        char web_ip[16] = {0};
        esp_err_t web_ip_err = wifi_manager_get_ip(web_ip, sizeof(web_ip));
        ESP_LOGI(TAG, "[WEB] Configuration UI: http://%s/",
                 (web_ip_err == ESP_OK) ? web_ip : "192.168.4.1");
    }

    ESP_LOGW(TAG, "[BOOT] Skeleton build: configuration, Wi-Fi and audio capture work; telephony does not.");
    ESP_LOGI(TAG, "[BOOT] Boot sequence complete, idling.");
}
