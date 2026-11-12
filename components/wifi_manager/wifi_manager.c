#include "wifi_manager.h"

#include <string.h>

#include "config_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "WIFI";

#define RECONNECT_BASE_MS   2000
#define RECONNECT_MAX_MS    60000
#define RECONNECT_MAX_SHIFT 5

/* Failed reconnect attempts before the setup AP is raised at runtime. */
#define AP_FALLBACK_AFTER_RETRIES 4

static esp_netif_t *s_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static esp_timer_handle_t s_retry_timer;

/* Written by the event/timer tasks, read by the application tasks. */
static volatile bool s_connected;
static volatile bool s_manual_disconnect;
static volatile bool s_ap_active;
static bool s_started;
static bool s_initialized;
static bool s_has_credentials;
static uint32_t s_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void retry_timer_cb(void *arg);

static void stop_retry_timer(void)
{
    if (s_retry_timer != NULL) {
        /* Returns ESP_ERR_INVALID_STATE when not running; that is fine. */
        esp_timer_stop(s_retry_timer);
    }
}

static void schedule_reconnect(void)
{
    if (s_manual_disconnect || s_retry_timer == NULL) {
        return;
    }

    uint32_t shift = (s_retry_count < RECONNECT_MAX_SHIFT) ? s_retry_count : RECONNECT_MAX_SHIFT;
    uint32_t delay_ms = RECONNECT_BASE_MS << shift;
    if (delay_ms > RECONNECT_MAX_MS) {
        delay_ms = RECONNECT_MAX_MS;
    }
    if (s_retry_count < UINT32_MAX) {
        s_retry_count++;
    }

    stop_retry_timer();
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to schedule reconnect: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "[WIFI] Reconnect attempt %u scheduled in %u ms",
             (unsigned)s_retry_count, (unsigned)delay_ms);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;

    if (s_manual_disconnect) {
        return;
    }

    /* Repeated failures: keep trying, but make the device reachable for setup. */
    if (!s_ap_active && s_retry_count >= AP_FALLBACK_AFTER_RETRIES) {
        ESP_LOGW(TAG, "[WIFI] %u failed attempts, enabling configuration AP",
                 (unsigned)s_retry_count);
        if (wifi_manager_start_fallback_ap() == ESP_OK) {
            /* The driver restart raises WIFI_EVENT_STA_START, which reconnects. */
            return;
        }
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[WIFI] Reconnect failed: %s", esp_err_to_name(err));
        schedule_reconnect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case WIFI_EVENT_STA_START: {
        ESP_LOGI(TAG, "[WIFI] Station started");
        esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[WIFI] Failed to disable power save: %s", esp_err_to_name(err));
        }
        if (!s_manual_disconnect && s_has_credentials) {
            err = esp_wifi_connect();
            if (err != ESP_OK) {
                /* A disconnect event normally follows and drives the retry. */
                ESP_LOGW(TAG, "[WIFI] Connect failed: %s", esp_err_to_name(err));
            }
        }
        break;
    }

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "[WIFI] Associated with AP, waiting for IP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        ESP_LOGW(TAG, "[WIFI] Disconnected (reason %d)",
                 (disc != NULL) ? (int)disc->reason : -1);
        schedule_reconnect();
        break;
    }

    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "[WIFI] Configuration AP '%s' is up (open, IP 192.168.4.1)",
                 WIFI_FALLBACK_AP_SSID);
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "[WIFI] Configuration AP stopped");
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    char ip[16] = {0};
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    if (event != NULL) {
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, (int)sizeof(ip));
    }

    s_retry_count = 0;
    s_connected = true;
    stop_retry_timer();

    ESP_LOGI(TAG, "[WIFI] Connected");
    ESP_LOGI(TAG, "[WIFI] IP: %s", (ip[0] != '\0') ? ip : "unknown");
    if (s_ap_active) {
        ESP_LOGI(TAG, "[WIFI] Configuration AP stays available at http://192.168.4.1");
    }
}

static esp_err_t apply_ip_config(void)
{
    const phone_config_t *cfg = config_manager_get();

    if (cfg->wifi.use_dhcp) {
        esp_err_t err = esp_netif_dhcpc_start(s_netif);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[WIFI] DHCP start failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "[WIFI] DHCP stop failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(cfg->wifi.ip, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->wifi.netmask, &ip_info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->wifi.gateway, &ip_info.gw) != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Invalid static IP configuration");
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_set_ip_info(s_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to set static IP: %s", esp_err_to_name(err));
        return err;
    }

    if (cfg->wifi.dns[0] != '\0') {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(cfg->wifi.dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            err = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[WIFI] Failed to set DNS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "[WIFI] Ignoring invalid DNS address");
        }
    }

    ESP_LOGI(TAG, "[WIFI] Using static IP %s", cfg->wifi.ip);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[WIFI] Event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "[WIFI] Failed to create station netif");
        return ESP_FAIL;
    }

    const phone_config_t *boot_cfg = config_manager_get();
    if (boot_cfg->system.hostname[0] != '\0') {
        esp_err_t host_err = esp_netif_set_hostname(s_netif, boot_cfg->system.hostname);
        if (host_err != ESP_OK) {
            ESP_LOGW(TAG, "[WIFI] Failed to set hostname '%s': %s",
                     boot_cfg->system.hostname, esp_err_to_name(host_err));
        } else {
            ESP_LOGI(TAG, "[WIFI] Hostname: %s", boot_cfg->system.hostname);
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Credentials live in config_manager's NVS namespace, not in the driver. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] esp_wifi_set_storage failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &ip_event_handler, NULL, &s_ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_timer_cb,
        .arg = NULL,
        .name = "wifi_retry",
        .skip_unhandled_events = false,
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to create retry timer: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "[WIFI] Initialised (station mode)");
    return ESP_OK;
}

#define WIFI_SCAN_MAX_RECORDS 24

static wifi_ap_record_t s_scan_records[WIFI_SCAN_MAX_RECORDS];

int wifi_manager_scan(wifi_scan_ap_t *out, size_t max)
{
    if (!s_initialized || out == NULL || max == 0) {
        return -1;
    }

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[WIFI] Scan start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        return 0;
    }
    if (count > WIFI_SCAN_MAX_RECORDS) {
        count = WIFI_SCAN_MAX_RECORDS;
    }
    if (esp_wifi_scan_get_ap_records(&count, s_scan_records) != ESP_OK) {
        return -1;
    }

    int found = 0;
    for (uint16_t i = 0; i < count && (size_t)found < max; i++) {
        if (s_scan_records[i].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < found; j++) {
            if (strcmp(out[j].ssid, (const char *)s_scan_records[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        strncpy(out[found].ssid, (const char *)s_scan_records[i].ssid,
                sizeof(out[found].ssid) - 1);
        out[found].ssid[sizeof(out[found].ssid) - 1] = '\0';
        out[found].rssi = s_scan_records[i].rssi;
        out[found].secure = s_scan_records[i].authmode != WIFI_AUTH_OPEN;
        found++;
    }

    /* Strongest first, like a phone's Wi-Fi picker. */
    for (int i = 1; i < found; i++) {
        wifi_scan_ap_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    ESP_LOGI(TAG, "[WIFI] Scan found %d network(s)", found);
    return found;
}

esp_err_t wifi_manager_start_fallback_ap(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ap_active) {
        return ESP_OK;
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "[WIFI] Failed to create configuration AP netif");
            return ESP_FAIL;
        }
    }

    wifi_config_t ap_cfg = {0};
    size_t ssid_len = strlen(WIFI_FALLBACK_AP_SSID);
    if (ssid_len > sizeof(ap_cfg.ap.ssid)) {
        ssid_len = sizeof(ap_cfg.ap.ssid);
    }
    memcpy(ap_cfg.ap.ssid, WIFI_FALLBACK_AP_SSID, ssid_len);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    /* Changing the mode requires the driver to be stopped first. */
    if (s_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "[WIFI] esp_wifi_stop failed: %s", esp_err_to_name(stop_err));
        }
        s_started = false;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to start configuration AP: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    s_ap_active = true;
    ESP_LOGW(TAG, "[WIFI] No usable upstream Wi-Fi: configuration AP '%s' is OPEN",
             WIFI_FALLBACK_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_fallback_ap(void)
{
    if (!s_initialized || !s_ap_active) {
        return ESP_OK;
    }

    if (s_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "[WIFI] esp_wifi_stop failed: %s", esp_err_to_name(stop_err));
        }
        s_started = false;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] Failed to leave AP mode: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    s_ap_active = false;
    ESP_LOGI(TAG, "[WIFI] Configuration AP stopped");
    return ESP_OK;
}

bool wifi_manager_is_ap_active(void)
{
    return s_ap_active;
}

esp_err_t wifi_manager_connect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connected) {
        ESP_LOGI(TAG, "[WIFI] Already connected");
        return ESP_OK;
    }

    const phone_config_t *cfg = config_manager_get();
    if (cfg->wifi.ssid[0] == '\0') {
        s_has_credentials = false;
        ESP_LOGW(TAG, "[WIFI] No SSID configured, not connecting");
        return ESP_ERR_INVALID_STATE;
    }
    s_has_credentials = true;

    s_manual_disconnect = false;
    s_retry_count = 0;
    stop_retry_timer();

    esp_err_t err = apply_ip_config();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_cfg = {0};
    size_t ssid_len = strnlen(cfg->wifi.ssid, sizeof(wifi_cfg.sta.ssid));
    memcpy(wifi_cfg.sta.ssid, cfg->wifi.ssid, ssid_len);
    size_t pass_len = strnlen(cfg->wifi.password, sizeof(wifi_cfg.sta.password));
    memcpy(wifi_cfg.sta.password, cfg->wifi.password, pass_len);
    wifi_cfg.sta.threshold.authmode = (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WIFI] esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "[WIFI] Connecting to SSID '%s'", cfg->wifi.ssid);

    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(TAG, "[WIFI] esp_wifi_start failed: %s", esp_err_to_name(err));
            return err;
        }
        s_started = true;
        /* WIFI_EVENT_STA_START calls esp_wifi_connect() from the event task. */
    } else {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[WIFI] esp_wifi_connect failed: %s", esp_err_to_name(err));
            schedule_reconnect();
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_manual_disconnect = true;
    s_retry_count = 0;
    stop_retry_timer();
    s_connected = false;

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[WIFI] esp_wifi_disconnect failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[WIFI] Disconnected by request");
    }
    return err;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return -127;
    }
    return ap.rssi;
}

esp_err_t wifi_manager_get_ip(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0 || s_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err = esp_netif_get_ip_info(s_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ip4addr_ntoa(&ip_info.ip, buf, (int)buf_len);
    return ESP_OK;
}
