# XIAO ESP32-S3 Sense SIP IP Phone

Firmware project for a desk SIP phone built on the **Seeed Studio XIAO ESP32-S3 Sense** with **ESP-IDF**. Target: register against FreePBX/Asterisk, place and answer calls, and exchange full-duplex G.711 audio, configured from a web browser.

The full requirements document is [`promp.txt`](promp.txt); project context is in [`knowledge.md`](knowledge.md).

## Status: configuration and audio work; telephony does not

The firmware boots, joins Wi-Fi, runs the PDM microphone, and serves a browser configuration UI with a REST API. **SIP and RTP are not implemented, so no calls can be placed or received.**

| Area | State |
| --- | --- |
| Project layout, component graph, partition table, sdkconfig defaults | Done |
| Boot banner, chip/PSRAM/heap logging | Implemented |
| Configuration manager (NVS, defaults, save, factory reset) | Implemented |
| SIP registration / call signalling | Stub, returns `ESP_ERR_NOT_SUPPORTED` |
| RTP transport + jitter buffer | Stub |
| PDM capture / I2S playback | Implemented: 16 kHz PDM → 8 kHz PCM, I2S playback, gain/volume, dBFS meters; mic verified on hardware (~-44 dBFS ambient), speaker path initialised but not exercised |
| Buttons | Stub |
| Wi-Fi station | Implemented: DHCP or static IP, backoff reconnect, power save off, open AP fallback `esp32SipPhone`; station driver initialised on hardware, AP fallback added |
| Web UI + REST API | Implemented: embedded dashboard and Wi-Fi/SIP/audio/system forms, Basic auth, NVS persistence, reboot/factory reset |

Nothing in the stubs simulates SIP or RTP: they log that nothing was sent, per the project rule against fake call paths.

## Layout

```
esp32_sip_phone/
├── CMakeLists.txt          ESP-IDF project entry
├── sdkconfig.defaults      esp32s3, octal PSRAM, 8 MB flash, 1 kHz tick
├── partitions.csv          NVS + dual OTA slots + SPIFFS
├── main/
│   ├── main.c              boot sequence and init reporting
│   ├── app_config.h        firmware identity, shared constants
│   ├── app_state.h/.c      thread-safe runtime state
│   └── CMakeLists.txt
├── components/
│   ├── sip_client/         SIP signalling facade
│   ├── rtp_audio/          rtp_client + rtp_jitter
│   ├── audio_driver/       audio_input, audio_output, audio_process
│   ├── phone_control/      call orchestration + button
│   ├── wifi_manager/       station management
│   ├── web_server/         httpd + REST API
│   └── config_manager/     NVS-backed configuration
└── web/                    index.html, style.css, app.js
```

Each component owns its public header and `CMakeLists.txt`. `main` requires all of them, so the dependency graph is already wired for later phases.

## Hardware

| Function | Signal | GPIO |
| --- | --- | --- |
| I2S audio out | DOUT | 7 |
| I2S audio out | BCLK | 8 |
| I2S audio out | LRC | 9 |
| PDM microphone | DATA | 41 |
| PDM microphone | CLK | 42 |
| Button | ANSWER | 3 |
| Button | HANGUP | 2 |

Cautions:

- **GPIO3 is a strapping pin.** Do not wire a pull-down that keeps it low at reset, or the board will not boot.
- **GPIO7/8/9 are shared with the microSD slot.** Never start the SD SPI driver; it would take over the I2S pins.
- **PSRAM** is assumed to be the 8 MB octal part on the ESP32-S3R8 module. If your board reports no PSRAM, adjust `sdkconfig.defaults` and re-check `CONFIG_SPIRAM_MODE_OCT`.
- G.711 audio is 8 kHz, mono, 16-bit PCM, 20 ms frames (160 samples -> 80 bytes per packet).

## Wi-Fi configuration

The station manager connects with the SSID/password stored in NVS. Seed the first boot through menuconfig (**XIAO SIP Phone** menu) or in `sdkconfig.defaults`; after that, change them from the Wi-Fi tab of the web UI:

```ini
CONFIG_SIP_PHONE_WIFI_SSID="spd_wifi"
CONFIG_SIP_PHONE_WIFI_PASSWORD="sphhospital"
```

These values are only written when no configuration exists yet; once NVS holds a configuration, changing them has no effect. `idf.py -p <PORT> erase-flash` clears NVS and re-seeds from Kconfig (it also discards SIP settings).

The repository currently ships with these lab test credentials (`spd_wifi` / `sphhospital`) so the first boot can be verified on the test network; delete or replace them before sharing the project.

Behaviour:

- Station mode only. DHCP by default; static address/subnet/gateway/DNS come from `phone_wifi_config_t` with `use_dhcp = false`.
- Credentials stay in the `config_manager` NVS namespace; the Wi-Fi driver uses `WIFI_STORAGE_RAM` so it never keeps its own copy.
- Power save is disabled (`WIFI_PS_NONE`) after the station starts, because modem sleep adds latency to 20 ms audio.
- Disconnect triggers reconnect with exponential backoff: 2 s doubling to a 60 s cap, retrying indefinitely. The counter resets on `IP_EVENT_STA_GOT_IP`.
- The password is never logged; SSID, IP and RSSI are.

### Configuration AP fallback

If no SSID is stored, the station cannot get an IP within 10 s of the boot attempt, or four reconnect attempts fail (~14 s after a disconnect), the device also raises an **open** access point so it stays reachable for setup:

| Setting | Value |
| --- | --- |
| SSID | `esp32SipPhone` |
| Security | Open (no password) |
| Address | http://192.168.4.1 |

It runs in `WIFI_MODE_APSTA`, so station reconnects continue in the background; when the station eventually connects the AP stays up. API: `wifi_manager_start_fallback_ap()`, `wifi_manager_stop_fallback_ap()`, `wifi_manager_is_ap_active()`.

Typical fallback boot log:

```text
[WIFI] No SSID configured, starting configuration AP
[WIFI] No usable upstream Wi-Fi: configuration AP 'esp32SipPhone' is OPEN
[WIFI] Configuration AP 'esp32SipPhone' is up (open, IP 192.168.4.1)
```

> The setup AP serves the same web UI at http://192.168.4.1. It is unencrypted and the UI uses Basic auth over plain HTTP — treat it as a lab/setup network only.

## Audio bring-up

Both directions use the ESP-IDF 5.x I2S driver (`driver/i2s_std.h`, `driver/i2s_pdm.h`) and run in dedicated FreeRTOS tasks.

| Path | Configuration |
| --- | --- |
| Microphone | PDM RX, 16 kHz, mono, 16-bit, GPIO41 DATA / GPIO42 CLK |
| Speaker | I2S standard (Philips) TX, 8 kHz, mono, 16-bit, GPIO8 BCLK / GPIO9 LRC / GPIO7 DOUT |

- Capture rounds 20 ms PDM frames (320 samples), decimates 2:1 with a two-tap average, removes the mic DC offset with a one-pole tracker, then applies mic gain. The result is 160-sample (20 ms) frames at 8 kHz.
- Frames move through FreeRTOS stream buffers (8 frames each). `audio_read_pcm()` waits at most 20 ms for a frame; `audio_write_pcm()` never blocks and drops the remainder when the ring is full.
- Mic gain (-24..+24 dB) and speaker volume (0..100) are software scaled and adjustable at runtime via `audio_set_mic_gain_db()` / `audio_set_volume()`.
- Every I2S access is bounded (50 ms), and the audio tasks run above SIP/network priority, so one direction cannot stall the other.
- Capture starts at boot, so the serial log shows a live mic meter without a call:

```text
[AUDIO] Drivers ready (mic gain 0 dB, volume 80%)
[AUDIO] Microphone: active (PDM 16000 Hz -> PCM 8000 Hz)
[AUDIO] Mic     [========            ]  -24 dBFS (peak  -18 dBFS)
```

The speaker meter appears when audio is written to the output path (a call, or a future test tone).

Limitations for this phase: no AEC or noise reduction, the two-tap decimation is not a proper anti-alias filter, DMA depth/latency is untuned on hardware, and mono slot mode assumes an I2S DAC/amp that accepts a single populated slot.

## Web configuration

With a station connection, browse to `http://<device-ip>/`; with no upstream Wi-Fi, join `esp32SipPhone` and open http://192.168.4.1. The default login is `admin` / `admin` (change it on the System tab). The UI is embedded in the firmware from `web/`.

| Page | What it does |
| --- | --- |
| Dashboard | Uptime, heap/PSRAM, Wi-Fi/IP/RSSI, setup-AP state, SIP/call state (SIP is a stub) |
| Wi-Fi | SSID/password, DHCP or static IP; saves to NVS and restarts the station |
| SIP | Server, port, username/auth ID/password, extension, transport, expiry, proxy, STUN — persisted only, no stack yet |
| Audio | Mic gain, speaker volume, jitter frames, NR/AEC preferences (applied immediately) |
| System | Device name/hostname, admin username/password, reboot, factory reset |

REST endpoints (HTTP Basic auth required on every route):

| Method | Path | Purpose |
| --- | --- |
| GET | `/api/status` | Live status JSON |
| GET | `/api/config` | Stored configuration; passwords are reported only as `password_set` |
| POST | `/api/config/wifi` | Save Wi-Fi; an empty password keeps the stored one |
| POST | `/api/config/audio` | Save and apply audio settings |
| POST | `/api/config/sip` | Save the SIP account |
| POST | `/api/config/admin` | Save device name/hostname and admin credentials |
| POST | `/api/system` | `{"action":"reboot"}` or `{"action":"factory_reset"}` |

Notes:

- Every route, including the HTML/CSS/JS, returns `401` with a `WWW-Authenticate` challenge until valid credentials are supplied.
- Stored passwords are never returned by the API; leaving a password field blank in the UI keeps the existing value.
- Wi-Fi changes restart the station; if the new network cannot be joined, the `esp32SipPhone` AP appears again within ~14 s.
- New admin credentials apply on the next request; the stored hostname is used on the next boot.
- Basic auth is sent over plain HTTP, and there is no CSRF token or session timeout yet. Use a trusted LAN and change the default password.

Verified on hardware (2026-11-09, COM8, `192.168.1.171`): unauthenticated and wrong-password requests both returned `401`; `/` served the HTML; `/api/status` reported the live IP/RSSI/heap; `POST /api/config/audio` changed `speaker_volume` to 55 and a follow-up `GET /api/config` read it back.

## Build and flash

Requires ESP-IDF v5.x (`idf.py` on `PATH`, typically after `. $IDF_PATH/export.sh`).

```bash
idf.py set-target esp32s3     # applies sdkconfig.defaults on first configure
idf.py menuconfig             # optional: verify PSRAM/flash settings
idf.py build
idf.py -p <PORT> flash monitor
```

Expected serial output:

```text
========================================
       ESP32-S3 SIP IP Phone
========================================
[BOOT] Device Starting...
[BOOT] Board: XIAO ESP32-S3 Sense
[BOOT] Firmware: 0.1.0-scaffold
[BOOT] PSRAM: 8192 KB total, ...
[BOOT] config_manager: ready
[BOOT] Device name: XIAO SIP Phone
[SYSTEM] [SYSTEM] Configured SIP: 192.168.4.29:5060, extension 101
[WIFI] Initialised (station mode)
[BOOT] wifi_manager: ready
[BOOT] [WIFI] Connecting to WiFi...
[WIFI] Connecting to SSID 'lab-ap'
[WIFI] Associated with AP, waiting for IP
[WIFI] Connected
[WIFI] IP: 192.168.4.150
[BOOT] [WIFI] Connected, IP: 192.168.4.150, RSSI: -48 dBm
[AUDIO] Drivers ready (mic gain 0 dB, volume 80%)
[BOOT] audio_driver: ready
[AUDIO] Microphone: active (PDM 16000 Hz -> PCM 8000 Hz)
[BOOT] [AUDIO] Microphone capture running, level meter on serial
[AUDIO] Mic     [========            ]  -24 dBFS (peak  -18 dBFS)
[WEB] [WEB] HTTP server started on port 80
[BOOT] [BOOT] web_server: ready
[BOOT] [WEB] Configuration UI: http://192.168.4.150/
...
[BOOT] Skeleton build: configuration, Wi-Fi and audio capture work; telephony does not.
```

> Verified on hardware (2026-11-09) with ESP-IDF 5.5.6 on COM8: build succeeded (app image 0xDE4F0 bytes, 71% of the OTA slot free), firmware flashed and booted, 8 MB octal PSRAM detected, and the PDM mic meter reads ambient level. The web UI and REST API were exercised over the LAN with `curl` (results above). The speaker path was initialised but not exercised, and no SIP/RTP calls were placed.

## Default configuration

Written to NVS on first boot (namespace `phone_cfg`):

| Field | Default |
| --- | --- |
| SIP server / port | `192.168.4.29` / `5060` (UDP) |
| Username / auth ID / extension | `101` / `101` / `101` |
| Codec preference | PCMU first, PCMA accepted |
| Speaker volume / mic gain | 80 / 0 dB |
| Jitter buffer | 10 frames |
| Web admin | `admin` / `admin` |

All values are placeholders for a lab FreePBX and are meant to be replaced through the web UI.

## Known issues / security debt

- Web admin defaults to `admin`/`admin`; credentials are stored in NVS and sent over plain HTTP Basic auth. Change the default immediately, and add TLS plus a hashed password before any real deployment.
- Wi-Fi credentials are stored in NVS in plain text. They can be set from the Wi-Fi tab, and the API never echoes them.
- There is no CSRF token, session timeout or login rate limit on the web API yet.
- Audio has no AEC or noise reduction yet; the 2-tap decimation is not a real anti-alias filter, DMA latency is untuned, and the speaker is only enabled when the call layer starts it.
- ESP-IDF's install/export scripts refuse to run under Git Bash (MSYS check); build from a normal Windows terminal or use the workaround documented in `knowledge.md`.
- The `esp32SipPhone` fallback AP is open (no password) by design for initial setup. The UI still requires Basic auth, but the AP and all traffic are unencrypted.
- The SIP stack is not chosen or vendored yet. PJSIP is the leading candidate but must be validated for ESP32-S3 + this IDF version first; alternatives must be checked the same way.
- The SPIFFS partition is reserved but unused; web assets are embedded in the app image.
- No OTA implementation despite the dual-slot partition table.

## Next steps

1. **Phase 3 — SIP (blocked on stack choice):** vendor and validate a SIP stack (PJSIP is the leading candidate) for ESP32-S3 + ESP-IDF 5.5, then REGISTER against FreePBX. Testing needs the PBX address and credentials.
2. **Phase 4 — RTP:** PCMU/PCMA send/receive, jitter buffer, RFC 4733 DTMF, driven by the SDP negotiated in Phase 3.
3. **Phase 5 — integration:** SIP + RTP + audio path, call teardown and buffer ownership.
4. **Phase 7 — hardening:** buttons with debounce, system monitor, watchdog, memory soak, OTA.
5. **Phase 8 — release package:** binaries, wiring/config guide, test report, known issues.
