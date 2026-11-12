# Original project analysis

Analysis date: 2026-11-12. Source: `../ESP32SipPhone` (unchanged), copied into this repository with its Git history.

## What the original firmware is

An ESP-IDF 5.5 project for the **Seeed Studio XIAO ESP32-S3 Sense** that acts as a desk SIP phone:

| Layer | Component | What it does |
| --- | --- | --- |
| Config | `config_manager` | One NVS blob (`phone_cfg`, schema v1) for Wi-Fi, SIP, audio, phone and system settings; defaults on first boot; factory reset. |
| Network | `wifi_manager` | Station mode, DHCP/static, backoff reconnect, SSID/IP/RSSI queries, open setup AP fallback (`esp32SipPhone`, 192.168.4.1). |
| Signalling | `sip_client` | In-house SIP UA on lwIP: REGISTER with MD5 digest (`qop=auth` and RFC 2069), INVITE/ACK/CANCEL/BYE/OPTIONS, SDP offer/answer for PCMU (0) and PCMA (8), re-register on IP change. |
| Media | `rtp_audio` | UDP RTP, 20 ms G.711 frames, 10-frame jitter/reorder buffer, RFC 3550 jitter estimate, send/receive counters. |
| Audio | `audio_driver` | PDM RX 16 kHz decimated to 8 kHz PCM, I2S standard TX 8 kHz, mic gain, speaker volume, dBFS meters, per-direction FreeRTOS tasks. |
| Control | `phone_control` | Call state machine over `sip_client` + `rtp_audio`, ANSWER/HANGUP buttons (GPIO3/GPIO2) with debounce, speed dial. |
| UI | `web_server` + `web/` | `httpd` on port 80, embedded HTML/CSS/JS, Basic auth, `/api/status`, `/api/config/*`, `/api/system`, `/api/call`. |
| State | `app_state` (in `main`) | Mutex-protected snapshot of Wi-Fi/SIP/call state for the web UI and monitor. |

Verified on real hardware (2026-11-10, COM8): registration against a Yeastar UC120, incoming and outgoing calls, two-way G.711 PCMU/PCMA, remote BYE and local teardown, ANSWER/HANGUP button flows without a stack overflow. The full test record is in `knowledge.md` of the original repository.

## Constraints inherited from the original

- **Do not modify the original project** (verified: all work happened in `ESP32SipPhone_screen`).
- **Do not rewrite the SIP stack.** `sip_client`, `rtp_audio`, `config_manager`, `wifi_manager` and `web_server` are reused as-is in this phase.
- **Web configuration must keep working** and must share `config_manager` with the on-device UI.
- **No mocks.** Nothing in this tree reports a call or audio path that is not real.
- Audio tasks must never be blocked by UI/SIP work.

## What changes for the screen edition

| Area | Original | Screen edition |
| --- | --- | --- |
| Board | XIAO ESP32-S3 Sense | GUITION JC4827W543 (ESP32-S3, 4 MB flash, 8 MB PSRAM) |
| Display | none | 480x272 QSPI panel (NV3041A controller) + LEDC backlight |
| Input | 2 GPIO buttons | GT911 capacitive touch + LVGL widgets |
| UI | browser only | LVGL 9.3 on-device UI, web UI retained |
| Audio pins | PDM 41/42, I2S 7/8/9 | INMP441 on 6/7/15, onboard amp (Phase 3); legacy path disabled because GPIO8 is touch SDA |
| Flash | 8 MB, 3 MB OTA slots | 4 MB, 1.875 MB OTA slots |
| Buttons | GPIO3/GPIO2 | disabled: GPIO3 is the GT911 INT line |

The SIP/RTP/web/config modules keep their public headers, so an eventual audio migration only has to satisfy `audio_driver.h`.
