/*
 * Application-wide constants: firmware identity and shared sizing.
 *
 * Hardware wiring lives with the driver that owns it, because ESP-IDF
 * components cannot include headers from main/:
 *   audio_driver.h - I2S and PDM pins, G.711 framing
 *   button.h       - ANSWER/HANGUP GPIOs, debounce timing
 *   rtp_client.h   - RTP payload types and local port range
 *
 * Task stacks/priorities are defined by each component when its task is
 * created (Phase 1+), so they are not centralised here.
 */
#pragma once

#define APP_NAME             "ESP32-S3 SIP IP Phone"
#define APP_BOARD            "XIAO ESP32-S3 Sense"
#define APP_FIRMWARE_VERSION "0.2.3"

/* Shared application state */
#define APP_REMOTE_NUMBER_LEN 32
