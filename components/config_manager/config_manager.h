/*
 * Persistent phone configuration, stored as one NVS blob.
 *
 * Passwords live in NVS in plain text for now. Before shipping, enable NVS
 * encryption or store a hash for the web admin password (see README, Known
 * issues). Never log the sip/web password fields.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CONFIG_SCHEMA_VERSION 1

#define CFG_SSID_LEN      33
#define CFG_SECRET_LEN    65
#define CFG_ADDR_LEN      16
#define CFG_SERVER_LEN    64
#define CFG_ID_LEN        33
#define CFG_EXT_LEN       16
#define CFG_NAME_LEN      33

typedef enum {
    SIP_TRANSPORT_UDP = 0,
    SIP_TRANSPORT_TCP,
} sip_transport_t;

typedef enum {
    DTMF_MODE_RFC2833 = 0,
    DTMF_MODE_SIP_INFO,
    DTMF_MODE_INBAND,
} dtmf_mode_t;

typedef struct {
    char ssid[CFG_SSID_LEN];
    char password[CFG_SECRET_LEN];
    bool use_dhcp;
    char ip[CFG_ADDR_LEN];
    char gateway[CFG_ADDR_LEN];
    char netmask[CFG_ADDR_LEN];
    char dns[CFG_ADDR_LEN];
} phone_wifi_config_t;

typedef struct {
    char server[CFG_SERVER_LEN];
    uint16_t port;
    char username[CFG_ID_LEN];
    char password[CFG_SECRET_LEN];
    char auth_id[CFG_ID_LEN];
    char display_name[CFG_NAME_LEN];
    char extension[CFG_EXT_LEN];
    sip_transport_t transport;
    uint16_t register_expiry_s;
    char outbound_proxy[CFG_SERVER_LEN];
    char stun_server[CFG_SERVER_LEN];
    dtmf_mode_t dtmf_mode;
    bool prefer_pcma; /* false: PCMU first in SDP offer */
} sip_config_t;

typedef struct {
    int8_t mic_gain_db;
    uint8_t speaker_volume; /* 0-100 */
    uint8_t jitter_frames;
    uint8_t packet_ms;
    bool noise_reduction;
    bool aec_enabled;
} audio_config_t;

typedef struct {
    char default_number[CFG_EXT_LEN];
    char display_name[CFG_NAME_LEN];
    bool auto_answer;
    uint16_t auto_answer_delay_ms;
    uint16_t ring_duration_s;
    uint16_t call_timeout_s;
} phone_settings_t;

typedef struct {
    char device_name[CFG_NAME_LEN];
    char hostname[CFG_NAME_LEN];
    char web_user[CFG_ID_LEN];
    char web_password[CFG_SECRET_LEN];
} system_config_t;

/* The ringtone shares the speaker with the call audio, so its level is kept
 * apart from audio.speaker_volume. */
#define RING_CONFIG_MAGIC 0x52 /* 'R': these fields have been written once */

typedef struct {
    uint8_t magic;       /* RING_CONFIG_MAGIC, see config_manager_load() */
    uint8_t ringtone;    /* 0..4, see ringtone.h */
    uint8_t ring_volume; /* 0-100 */
    uint8_t reserved;
} ring_config_t;

/* The dialpad key tone has its own block: a field added inside ring_config_t
 * would land on the old padding byte, which reads back as a silent keypad.
 * A separate magic makes "never written" detectable. */
#define KEYTONE_CONFIG_MAGIC 0x4B /* 'K' */

typedef struct {
    uint8_t magic;      /* KEYTONE_CONFIG_MAGIC */
    uint8_t key_volume; /* 0-100, 0 = silent */
    uint8_t reserved[2];
} keytone_config_t;

typedef struct {
    uint16_t version; /* CONFIG_SCHEMA_VERSION, for future migration */
    phone_wifi_config_t wifi;
    sip_config_t sip;
    audio_config_t audio;
    phone_settings_t phone;
    system_config_t system;
    /* Appended after the original fields: a blob written by an older build is
     * a valid prefix and is migrated instead of discarded. */
    ring_config_t ring;
    keytone_config_t keytone;
} phone_config_t;

/* Initialises NVS (erasing/recovering it if needed), then loads the stored
 * configuration or writes defaults on first boot. */
esp_err_t config_manager_init(void);

esp_err_t config_manager_load(void);
esp_err_t config_manager_save(void);

/* Returns the live configuration. Treat as read-only; use
 * config_manager_set() to change it (it persists to NVS). */
const phone_config_t *config_manager_get(void);

/* True once NVS has been initialised and a configuration loaded or written. */
bool config_manager_is_ready(void);

esp_err_t config_manager_set(const phone_config_t *cfg);

/* Erases all stored configuration and restores defaults. */
esp_err_t config_manager_factory_reset(void);
