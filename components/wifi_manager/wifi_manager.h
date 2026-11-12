/*
 * Wi-Fi station manager with an open configuration AP fallback.
 *
 * Responsibilities:
 *   - esp_netif station interface, default event loop and Wi-Fi driver setup
 *   - connect using the SSID/password stored by config_manager
 *   - DHCP (default) or static address/subnet/gateway/DNS from configuration
 *   - automatic reconnect with exponential backoff when the AP disappears
 *   - when there is no usable upstream Wi-Fi, raise an OPEN setup AP named
 *     WIFI_FALLBACK_AP_SSID at http://192.168.4.1 (WIFI_MODE_APSTA, so STA
 *     retries continue in the background)
 *
 * Power save is disabled once the station starts, because modem sleep adds
 * latency that hurts 20 ms G.711 audio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Open configuration access point used when the station cannot connect. */
#define WIFI_FALLBACK_AP_SSID "esp32SipPhone"

/* Brings up the netif/driver and registers event handlers. Does not connect. */
esp_err_t wifi_manager_init(void);

/* Reads SSID/password from config_manager and starts associating.
 * Returns once the attempt is in flight; watch the WIFI-tag logs or poll
 * wifi_manager_is_connected() for the outcome. Safe to call to retry. */
esp_err_t wifi_manager_connect(void);

/* Stops reconnecting and drops the current association. */
esp_err_t wifi_manager_disconnect(void);

bool wifi_manager_is_connected(void);

/* Last associated AP RSSI in dBm, or -127 when not connected. */
int8_t wifi_manager_get_rssi(void);

/* Writes the current IPv4 address into buf as a string. */
esp_err_t wifi_manager_get_ip(char *buf, size_t buf_len);

#define WIFI_MANAGER_SCAN_MAX 12

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_ap_t;

/* Blocking scan (2-4 s) that leaves the driver running. Fills up to max
 * entries, strongest first, duplicates removed; returns the count or a
 * negative value when the scan could not run. */
int wifi_manager_scan(wifi_scan_ap_t *out, size_t max);

/* Switches to WIFI_MODE_APSTA and starts the open setup AP. Idempotent.
 * Restarts the Wi-Fi driver, so only call when the station is not carrying a
 * call. Normally invoked automatically when the station has no usable link. */
esp_err_t wifi_manager_start_fallback_ap(void);

/* Stops the setup AP and returns to station-only mode. */
esp_err_t wifi_manager_stop_fallback_ap(void);

bool wifi_manager_is_ap_active(void);
