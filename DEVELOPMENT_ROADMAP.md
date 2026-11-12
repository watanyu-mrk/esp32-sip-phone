# Development roadmap

Status as of 2026-11-12. Every phase must build; hardware claims are only made after a test on the actual JC4827W543.

## Phase 1 — Fork and analyse: DONE

- Original project inspected; no file in `../ESP32SipPhone` modified.
- New project `ESP32SipPhone_screen` created with the original Git history (4 commits).
- Board hardware resolved from vendor/community sources; see `HARDWARE_COMPATIBILITY.md`.
- Pin conflicts found and resolved by disabling the XIAO audio and button paths.
- Flash changed from 8 MB to the board's 4 MB, partition table rebuilt.
- LVGL 9.3.0 added as a managed dependency (manifest in `main/idf_component.yml`).
- Build passes for esp32s3 (ESP-IDF 5.5.6): `esp32_sip_phone.bin` ≈ 1.3 MB, 30% of the OTA slot free.

## Phase 2 — Display, touch, LVGL: IMPLEMENTED, NOT YET HARDWARE-TESTED

- `display_driver`: QSPI bus, NV3041A panel driver with the JC4827W543 init sequence, PWM backlight.
- `touch_driver`: GT911 reset/address strap, product-ID check, polling reads, coordinate mapping.
- `lvgl_port`: RGB565 partial buffers, QSPI flush with transfer-completion wait, 1 kHz tick, 8 KB task, mutex.
- `ui`: home screen with header (device, Ext, Wi-Fi, SIP), number display, 12-key dialpad, delete (tap and long-press), CALL routed through `phone_control`, bottom bar placeholders.
- Build verified. **Pending hardware:** first light, colour test, touch coordinate check, UI demo, frame rate under load.

## Phase 3 — Audio migration

- INMP441 I2S RX (GPIO6/7/15), amplifier I2S TX (pins confirmed from schematic).
- Replace the XIAO PDM/I2S implementation behind the existing `audio_driver.h` API.
- Validate mic level, speaker tone, full duplex, then a real call.

## Phase 4 — SIP integration on the new board

- Regression test the reused `sip_client`/`rtp_audio`: register, dial, answer, hangup, two-way audio, DTMF.
- Compare against the original feature set; nothing may be lost.

## Phase 5 — Full LVGL phone UI

- Incoming and active-call screens with real state and caller ID.
- Contacts and call history backed by NVS (batched writes, no per-keystroke persistence).
- SIP/Wi-Fi/audio/system settings screens sharing `config_manager` with the web UI.
- Thai font asset, screen sleep/backlight timeout, long-press behaviour.

## Phase 6 — Integration

- `phone_event_t` queue from `phone_control` to the UI (replaces polling).
- Web ↔ screen changes stay in sync through one config/state source.
- Ring tone, mute, DTMF keypad, volume controls during a call.

## Phase 7 — Stability

- Repeated incoming/outgoing calls, mute and DTMF during calls, touch during RTP.
- Wi-Fi loss/reconnect, UI + audio soak, memory leak checks, restart and factory reset.

## Phase 8 — Release

- Source, firmware binary, IDF configuration, pinout/wiring (INMP441), LVGL sources, README, build/flash instructions, test report.

## Git policy

One commit per phase (plus fixes), each with its build/test evidence. Phase 1+2 are committed together because Phase 1 produced no separate runnable firmware.
