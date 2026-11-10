#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_driver.h"
#include "cJSON.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sip_client.h"
#include "wifi_manager.h"

#include "web_internal.h"

static const char *TAG = "WEB";

/* Keep in sync with APP_FIRMWARE_VERSION in main/app_config.h. */
#define WEB_FIRMWARE_VERSION "0.2.3"

#define WEB_BODY_MAX   1024
#define WEB_AUTH_MAX   160
#define WEB_DECODE_MAX 192

static const char *reg_state_name(sip_reg_state_t state)
{
    switch (state) {
    case SIP_REG_STATE_UNREGISTERED: return "unregistered";
    case SIP_REG_STATE_REGISTERING:  return "registering";
    case SIP_REG_STATE_REGISTERED:   return "registered";
    case SIP_REG_STATE_FAILED:       return "failed";
    default:                         return "unknown";
    }
}

static const char *call_state_name(sip_call_state_t state)
{
    switch (state) {
    case SIP_CALL_IDLE:             return "idle";
    case SIP_CALL_DIALING:          return "dialing";
    case SIP_CALL_RINGING_OUTGOING: return "ringing_outgoing";
    case SIP_CALL_RINGING_INCOMING: return "ringing_incoming";
    case SIP_CALL_ESTABLISHED:      return "established";
    case SIP_CALL_TERMINATING:      return "terminating";
    default:                        return "unknown";
    }
}

static bool auth_send_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"XIAO SIP Phone\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

bool web_auth_check(httpd_req_t *req)
{
    char header[WEB_AUTH_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "Basic ", 6) != 0) {
        return auth_send_challenge(req);
    }

    unsigned char decoded[WEB_DECODE_MAX];
    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                   (const unsigned char *)header + 6, strlen(header + 6));
    if (rc != 0) {
        return auth_send_challenge(req);
    }
    decoded[decoded_len] = '\0';

    const phone_config_t *cfg = config_manager_get();
    char expected[CFG_ID_LEN + CFG_SECRET_LEN + 2];
    int expected_len = snprintf(expected, sizeof(expected), "%s:%s",
                                cfg->system.web_user, cfg->system.web_password);
    if (expected_len <= 0) {
        return auth_send_challenge(req);
    }

    bool match = ((size_t)expected_len == decoded_len);
    if (match) {
        unsigned diff = 0;
        for (size_t i = 0; i < decoded_len; i++) {
            diff |= (unsigned)(decoded[i] ^ (unsigned char)expected[i]);
        }
        match = (diff == 0);
    }

    if (!match) {
        return auth_send_challenge(req);
    }
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status_line)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t json_ok(httpd_req_t *req, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (message != NULL) {
        cJSON_AddStringToObject(root, "message", message);
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t json_error(httpd_req_t *req, const char *status_line, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, root, status_line);
}

static bool read_json_body(httpd_req_t *req, char *buf, size_t buf_len, cJSON **out)
{
    size_t total = req->content_len;
    if (total == 0 || total > buf_len - 1) {
        return false;
    }

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            return false;
        }
        received += (size_t)r;
    }
    buf[total] = '\0';

    *out = cJSON_Parse(buf);
    return *out != NULL;
}

static void copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dst_len, "%s", item->valuestring);
    }
}

static bool json_bool(cJSON *root, const char *key, bool current)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(item)) {
        return current;
    }
    return item->valueint != 0;
}

static int json_int(cJSON *root, const char *key, int current)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : current;
}

static int clamp_int(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static esp_err_t api_status_get(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    const phone_config_t *cfg = config_manager_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ip[16] = {0};
    if (wifi_manager_get_ip(ip, sizeof(ip)) != ESP_OK) {
        ip[0] = '\0';
    }

    cJSON_AddStringToObject(root, "device_name", cfg->system.device_name);
    cJSON_AddStringToObject(root, "firmware", WEB_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "extension", cfg->sip.extension);
    cJSON_AddStringToObject(root, "sip_server", cfg->sip.server);
    cJSON_AddStringToObject(root, "ssid", cfg->wifi.ssid);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "sip_state", reg_state_name(sip_client_get_reg_state()));
    cJSON_AddStringToObject(root, "call_state", call_state_name(sip_client_get_call_state()));
    cJSON_AddStringToObject(root, "ap_ssid", WIFI_FALLBACK_AP_SSID);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "ap_active", wifi_manager_is_ap_active());
    cJSON_AddNumberToObject(root, "wifi_rssi", (double)wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    return send_json(req, root, "200 OK");
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    const phone_config_t *cfg = config_manager_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi.ssid);
    cJSON_AddBoolToObject(wifi, "use_dhcp", cfg->wifi.use_dhcp);
    cJSON_AddStringToObject(wifi, "ip", cfg->wifi.ip);
    cJSON_AddStringToObject(wifi, "netmask", cfg->wifi.netmask);
    cJSON_AddStringToObject(wifi, "gateway", cfg->wifi.gateway);
    cJSON_AddStringToObject(wifi, "dns", cfg->wifi.dns);
    cJSON_AddBoolToObject(wifi, "password_set", cfg->wifi.password[0] != '\0');

    cJSON *sip = cJSON_AddObjectToObject(root, "sip");
    cJSON_AddStringToObject(sip, "server", cfg->sip.server);
    cJSON_AddNumberToObject(sip, "port", cfg->sip.port);
    cJSON_AddStringToObject(sip, "username", cfg->sip.username);
    cJSON_AddStringToObject(sip, "auth_id", cfg->sip.auth_id);
    cJSON_AddStringToObject(sip, "display_name", cfg->sip.display_name);
    cJSON_AddStringToObject(sip, "extension", cfg->sip.extension);
    cJSON_AddStringToObject(sip, "transport",
                            (cfg->sip.transport == SIP_TRANSPORT_TCP) ? "tcp" : "udp");
    cJSON_AddNumberToObject(sip, "register_expiry_s", cfg->sip.register_expiry_s);
    cJSON_AddStringToObject(sip, "outbound_proxy", cfg->sip.outbound_proxy);
    cJSON_AddStringToObject(sip, "stun_server", cfg->sip.stun_server);
    cJSON_AddBoolToObject(sip, "prefer_pcma", cfg->sip.prefer_pcma);
    cJSON_AddBoolToObject(sip, "password_set", cfg->sip.password[0] != '\0');

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddNumberToObject(audio, "mic_gain_db", cfg->audio.mic_gain_db);
    cJSON_AddNumberToObject(audio, "speaker_volume", cfg->audio.speaker_volume);
    cJSON_AddNumberToObject(audio, "jitter_frames", cfg->audio.jitter_frames);
    cJSON_AddNumberToObject(audio, "packet_ms", cfg->audio.packet_ms);
    cJSON_AddBoolToObject(audio, "noise_reduction", cfg->audio.noise_reduction);
    cJSON_AddBoolToObject(audio, "aec_enabled", cfg->audio.aec_enabled);

    cJSON *phone = cJSON_AddObjectToObject(root, "phone");
    cJSON_AddStringToObject(phone, "default_number", cfg->phone.default_number);
    cJSON_AddStringToObject(phone, "display_name", cfg->phone.display_name);
    cJSON_AddBoolToObject(phone, "auto_answer", cfg->phone.auto_answer);
    cJSON_AddNumberToObject(phone, "auto_answer_delay_ms", cfg->phone.auto_answer_delay_ms);
    cJSON_AddNumberToObject(phone, "ring_duration_s", cfg->phone.ring_duration_s);
    cJSON_AddNumberToObject(phone, "call_timeout_s", cfg->phone.call_timeout_s);
    cJSON_AddStringToObject(phone, "dtmf_mode",
                            (cfg->sip.dtmf_mode == DTMF_MODE_SIP_INFO)    ? "info"
                            : (cfg->sip.dtmf_mode == DTMF_MODE_INBAND)    ? "inband"
                                                                          : "rfc2833");

    cJSON *system = cJSON_AddObjectToObject(root, "system");
    cJSON_AddStringToObject(system, "device_name", cfg->system.device_name);
    cJSON_AddStringToObject(system, "hostname", cfg->system.hostname);
    cJSON_AddStringToObject(system, "web_user", cfg->system.web_user);
    cJSON_AddBoolToObject(system, "web_password_set", cfg->system.web_password[0] != '\0');

    return send_json(req, root, "200 OK");
}

static esp_err_t api_config_wifi_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "ssid is required");
    }

    phone_config_t cfg = *config_manager_get();
    snprintf(cfg.wifi.ssid, sizeof(cfg.wifi.ssid), "%s", ssid->valuestring);

    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(password) && password->valuestring != NULL &&
        password->valuestring[0] != '\0') {
        snprintf(cfg.wifi.password, sizeof(cfg.wifi.password), "%s", password->valuestring);
    }

    cfg.wifi.use_dhcp = json_bool(root, "use_dhcp", cfg.wifi.use_dhcp);
    copy_json_string(root, "ip", cfg.wifi.ip, sizeof(cfg.wifi.ip));
    copy_json_string(root, "netmask", cfg.wifi.netmask, sizeof(cfg.wifi.netmask));
    copy_json_string(root, "gateway", cfg.wifi.gateway, sizeof(cfg.wifi.gateway));
    copy_json_string(root, "dns", cfg.wifi.dns, sizeof(cfg.wifi.dns));
    cJSON_Delete(root);

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        return json_error(req, "500 Internal Server Error", "failed to save configuration");
    }

    wifi_manager_disconnect();
    err = wifi_manager_connect();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return json_error(req, "500 Internal Server Error", "saved, but reconnect failed");
    }

    ESP_LOGI(TAG, "[WEB] Wi-Fi configuration updated (SSID '%s')", cfg.wifi.ssid);
    return json_ok(req, "Wi-Fi saved; reconnecting");
}

static esp_err_t api_config_audio_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    phone_config_t cfg = *config_manager_get();
    cfg.audio.mic_gain_db = (int8_t)clamp_int(
        json_int(root, "mic_gain_db", cfg.audio.mic_gain_db), -24, 24);
    cfg.audio.speaker_volume = (uint8_t)clamp_int(
        json_int(root, "speaker_volume", cfg.audio.speaker_volume), 0, 100);
    cfg.audio.jitter_frames = (uint8_t)clamp_int(
        json_int(root, "jitter_frames", cfg.audio.jitter_frames), 2, 50);
    cfg.audio.noise_reduction = json_bool(root, "noise_reduction", cfg.audio.noise_reduction);
    cfg.audio.aec_enabled = json_bool(root, "aec_enabled", cfg.audio.aec_enabled);
    cJSON_Delete(root);

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        return json_error(req, "500 Internal Server Error", "failed to save configuration");
    }

    audio_set_mic_gain_db(cfg.audio.mic_gain_db);
    audio_set_volume(cfg.audio.speaker_volume);

    ESP_LOGI(TAG, "[WEB] Audio configuration updated");
    return json_ok(req, "Audio settings saved");
}

static esp_err_t api_config_sip_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    phone_config_t cfg = *config_manager_get();
    copy_json_string(root, "server", cfg.sip.server, sizeof(cfg.sip.server));
    copy_json_string(root, "username", cfg.sip.username, sizeof(cfg.sip.username));
    copy_json_string(root, "auth_id", cfg.sip.auth_id, sizeof(cfg.sip.auth_id));
    copy_json_string(root, "display_name", cfg.sip.display_name, sizeof(cfg.sip.display_name));
    copy_json_string(root, "extension", cfg.sip.extension, sizeof(cfg.sip.extension));
    copy_json_string(root, "outbound_proxy", cfg.sip.outbound_proxy,
                     sizeof(cfg.sip.outbound_proxy));
    copy_json_string(root, "stun_server", cfg.sip.stun_server, sizeof(cfg.sip.stun_server));

    cfg.sip.port = (uint16_t)clamp_int(json_int(root, "port", cfg.sip.port), 1, 65535);
    cfg.sip.register_expiry_s = (uint16_t)clamp_int(
        json_int(root, "register_expiry_s", cfg.sip.register_expiry_s), 30, 3600);
    cfg.sip.prefer_pcma = json_bool(root, "prefer_pcma", cfg.sip.prefer_pcma);

    cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    if (cJSON_IsString(transport) && transport->valuestring != NULL) {
        cfg.sip.transport = (strcmp(transport->valuestring, "tcp") == 0)
                                ? SIP_TRANSPORT_TCP
                                : SIP_TRANSPORT_UDP;
    }

    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(password) && password->valuestring != NULL &&
        password->valuestring[0] != '\0') {
        snprintf(cfg.sip.password, sizeof(cfg.sip.password), "%s", password->valuestring);
    }
    cJSON_Delete(root);

    if (cfg.sip.server[0] == '\0' || cfg.sip.extension[0] == '\0') {
        return json_error(req, "400 Bad Request", "server and extension are required");
    }

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        return json_error(req, "500 Internal Server Error", "failed to save configuration");
    }

    ESP_LOGI(TAG, "[WEB] SIP configuration updated (server '%s', extension '%s')",
             cfg.sip.server, cfg.sip.extension);
    return json_ok(req, "SIP settings saved; reboot to apply");
}

static esp_err_t api_config_admin_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    phone_config_t cfg = *config_manager_get();
    char user[CFG_ID_LEN];
    snprintf(user, sizeof(user), "%s", cfg.system.web_user);
    copy_json_string(root, "username", user, sizeof(user));
    if (user[0] == '\0') {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "username is required");
    }
    snprintf(cfg.system.web_user, sizeof(cfg.system.web_user), "%s", user);

    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(password) && password->valuestring != NULL &&
        password->valuestring[0] != '\0') {
        snprintf(cfg.system.web_password, sizeof(cfg.system.web_password), "%s",
                 password->valuestring);
    }

    cJSON *device_name = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    if (cJSON_IsString(device_name) && device_name->valuestring != NULL &&
        device_name->valuestring[0] != '\0') {
        snprintf(cfg.system.device_name, sizeof(cfg.system.device_name), "%s",
                 device_name->valuestring);
    }

    cJSON *hostname = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (cJSON_IsString(hostname) && hostname->valuestring != NULL &&
        hostname->valuestring[0] != '\0') {
        snprintf(cfg.system.hostname, sizeof(cfg.system.hostname), "%s",
                 hostname->valuestring);
    }
    cJSON_Delete(root);

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        return json_error(req, "500 Internal Server Error", "failed to save configuration");
    }

    ESP_LOGI(TAG, "[WEB] Device settings updated (name '%s', host '%s', admin '%s')",
             cfg.system.device_name, cfg.system.hostname, cfg.system.web_user);
    return json_ok(req, "Device settings saved. New credentials apply to the next request.");
}

static bool speed_dial_number_valid(const char *number)
{
    if (number == NULL) {
        return false;
    }
    for (const char *p = number; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p) && *p != '*' && *p != '#' && *p != '+') {
            return false;
        }
    }
    return true;
}

static esp_err_t api_config_phone_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    phone_config_t cfg = *config_manager_get();

    char number[CFG_EXT_LEN];
    snprintf(number, sizeof(number), "%s", cfg.phone.default_number);
    copy_json_string(root, "default_number", number, sizeof(number));

    /* Allow spaces around the number, reject anything that is not dialable. */
    char clean[CFG_EXT_LEN];
    size_t out = 0;
    for (const char *p = number; *p != '\0' && out + 1 < sizeof(clean); p++) {
        if (*p != ' ') {
            clean[out++] = *p;
        }
    }
    clean[out] = '\0';
    if (clean[0] != '\0' && !speed_dial_number_valid(clean)) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request",
                          "speed-dial number may only contain digits, *, # and +");
    }
    snprintf(cfg.phone.default_number, sizeof(cfg.phone.default_number), "%s", clean);

    copy_json_string(root, "display_name", cfg.phone.display_name, sizeof(cfg.phone.display_name));
    cfg.phone.auto_answer = json_bool(root, "auto_answer", cfg.phone.auto_answer);
    cfg.phone.auto_answer_delay_ms = (uint16_t)clamp_int(
        json_int(root, "auto_answer_delay_ms", cfg.phone.auto_answer_delay_ms), 0, 10000);
    cfg.phone.ring_duration_s = (uint16_t)clamp_int(
        json_int(root, "ring_duration_s", cfg.phone.ring_duration_s), 5, 120);
    cfg.phone.call_timeout_s = (uint16_t)clamp_int(
        json_int(root, "call_timeout_s", cfg.phone.call_timeout_s), 0, 3600);

    cJSON *dtmf = cJSON_GetObjectItemCaseSensitive(root, "dtmf_mode");
    if (cJSON_IsString(dtmf) && dtmf->valuestring != NULL) {
        if (strcmp(dtmf->valuestring, "info") == 0) {
            cfg.sip.dtmf_mode = DTMF_MODE_SIP_INFO;
        } else if (strcmp(dtmf->valuestring, "inband") == 0) {
            cfg.sip.dtmf_mode = DTMF_MODE_INBAND;
        } else {
            cfg.sip.dtmf_mode = DTMF_MODE_RFC2833;
        }
    }
    cJSON_Delete(root);

    esp_err_t err = config_manager_set(&cfg);
    if (err != ESP_OK) {
        return json_error(req, "500 Internal Server Error", "failed to save configuration");
    }

    ESP_LOGI(TAG, "[WEB] Phone settings updated (speed dial '%s', auto answer %d)",
             cfg.phone.default_number, (int)cfg.phone.auto_answer);
    return json_ok(req, "Phone settings saved");
}

static esp_err_t api_system_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "action is required");
    }

    bool reboot = (strcmp(action->valuestring, "reboot") == 0);
    bool factory_reset = (strcmp(action->valuestring, "factory_reset") == 0);
    cJSON_Delete(root);

    if (!reboot && !factory_reset) {
        return json_error(req, "400 Bad Request", "unknown action");
    }

    if (factory_reset) {
        esp_err_t err = config_manager_factory_reset();
        if (err != ESP_OK) {
            return json_error(req, "500 Internal Server Error", "factory reset failed");
        }
        ESP_LOGW(TAG, "[WEB] Factory reset requested");
        json_ok(req, "Factory reset complete; restarting");
    } else {
        ESP_LOGW(TAG, "[WEB] Reboot requested");
        json_ok(req, "Restarting");
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_call_post(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }

    char body[WEB_BODY_MAX + 1];
    cJSON *root = NULL;
    if (!read_json_body(req, body, sizeof(body), &root)) {
        return json_error(req, "400 Bad Request", "invalid JSON body");
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "action is required");
    }

    char act[16];
    snprintf(act, sizeof(act), "%s", action->valuestring);
    char number[CFG_EXT_LEN] = {0};
    copy_json_string(root, "number", number, sizeof(number));
    cJSON_Delete(root);

    esp_err_t err;
    if (strcmp(act, "dial") == 0) {
        if (number[0] == '\0') {
            /* No number given: use the speed-dial number from the Phone tab. */
            const phone_config_t *cfg = config_manager_get();
            snprintf(number, sizeof(number), "%s", cfg->phone.default_number);
        }
        if (number[0] == '\0') {
            return json_error(req, "400 Bad Request", "number is required to dial");
        }
        err = sip_client_dial(number);
    } else if (strcmp(act, "answer") == 0) {
        err = sip_client_answer();
    } else if (strcmp(act, "hangup") == 0 || strcmp(act, "reject") == 0) {
        err = sip_client_hangup();
    } else {
        return json_error(req, "400 Bad Request", "unknown action");
    }

    if (err != ESP_OK) {
        return json_error(req, "409 Conflict", esp_err_to_name(err));
    }
    return json_ok(req, act);
}

esp_err_t web_api_init(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get, .user_ctx = NULL },
        { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get, .user_ctx = NULL },
        { .uri = "/api/config/wifi", .method = HTTP_POST, .handler = api_config_wifi_post, .user_ctx = NULL },
        { .uri = "/api/config/audio", .method = HTTP_POST, .handler = api_config_audio_post, .user_ctx = NULL },
        { .uri = "/api/config/sip", .method = HTTP_POST, .handler = api_config_sip_post, .user_ctx = NULL },
        { .uri = "/api/config/admin", .method = HTTP_POST, .handler = api_config_admin_post, .user_ctx = NULL },
        { .uri = "/api/config/phone", .method = HTTP_POST, .handler = api_config_phone_post, .user_ctx = NULL },
        { .uri = "/api/system", .method = HTTP_POST, .handler = api_system_post, .user_ctx = NULL },
        { .uri = "/api/call", .method = HTTP_POST, .handler = api_call_post, .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[WEB] Failed to register %s: %s", routes[i].uri,
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}
