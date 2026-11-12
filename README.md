# GUITION JC4827W543 SIP IP Phone (screen edition)

Touchscreen fork of the XIAO ESP32-S3 Sense SIP phone, retargeted to the **GUITION JC4827W543** (ESP32-S3, 4.3" 480x272 QSPI panel, GT911 touch) with an on-device **LVGL 9.3** UI. The SIP/RTP/config/web components are inherited unchanged.

> **Status 2026-11-12:** Phase 1 (fork/hardware analysis), Phase 2 (display, touch, LVGL home screen) and Phase 3 (INMP441 + onboard amplifier audio backend) are implemented and the firmware builds for esp32s3, but **nothing has been tested on this board yet** — no display, touch or audio hardware was attached. The default audio backend is `SIP_PHONE_AUDIO_INMP441`; its amplifier pins (42/2/41) are unconfirmed and the microphone is not wired. See `DEVELOPMENT_ROADMAP.md`, `BOARD_PIN_MAPPING.md` and `AUDIO_MIGRATION_PLAN.md`; the sections below describe the inherited SIP/web behaviour on the original XIAO board.

Firmware project for a desk SIP phone built on the **Seeed Studio XIAO ESP32-S3 Sense** with **ESP-IDF**. Target: register against FreePBX/Asterisk, place and answer calls, and exchange full-duplex G.711 audio, configured from a web browser.

The full requirements document is [`promp.txt`](promp.txt); project context is in [`knowledge.md`](knowledge.md).

## Status: registers and carries calls with G.711 audio

The firmware boots, joins Wi-Fi, starts the PDM microphone, and serves the browser UI. It registers to a real PBX (Yeastar UC120, `192.168.5.142`, extension `901`) with digest authentication, authenticates outgoing INVITEs with the same credentials, accepts incoming INVITEs, answers with SDP, exchanges 20 ms G.711 RTP in both directions, and tears the call down with BYE. The two buttons answer, reject and hang up, and `POST /api/call` drives the same actions for testing.

The pre-compiled `esp_sip_service`/`esp_rtc` stack was rejected on this hardware: after the first REGISTER it re-read the same 262-byte response in a tight loop, never sent the authenticated retry, and starved CPU0 until the task watchdog fired. `components/sip_client` therefore implements the SIP subset this phone needs directly on lwIP (REGISTER, INVITE, ACK, CANCEL, BYE, OPTIONS, MD5 digest with and without `qop=auth`, SDP for PCMU/PCMA). That is a real protocol implementation, not a simulator.

| Area | State |
| --- | --- |
| Project layout, component graph, partition table, sdkconfig defaults | Done |
| Boot banner, chip/PSRAM/heap logging | Implemented |
| Configuration manager (NVS, defaults, save, factory reset) | Implemented |
| SIP registration / call signalling | Implemented: in-house UA on lwIP, REGISTER and INVITE with digest (qop or legacy), refresh/backoff, INVITE/ACK/CANCEL/BYE/OPTIONS, SDP; registers to the UC120 as `901` |
| RTP transport + jitter buffer | Implemented: PCMU/PCMA, 20 ms packets, sequence/timestamp/SSRC, RFC 3550 jitter estimate, 10-frame reorder buffer, live send/receive counters |
| PDM capture / I2S playback | Implemented: 16 kHz PDM → 8 kHz PCM, I2S playback, gain/volume, dBFS meters; both directions verified in a call (a 1 kHz test tone played on the speaker was picked up by the mic at −6 dBFS and sent back over RTP) |
| Buttons | Implemented: GPIO3 answer/dial-default, GPIO2 reject/cancel/hang up, 30 ms debounce, 1.5 s long press |
| Wi-Fi station | Implemented: DHCP or static IP, backoff reconnect, power save off, open AP fallback `esp32SipPhone`; station driver initialised on hardware, AP fallback added |
| Web UI + REST API | Implemented: embedded dashboard and Wi-Fi/SIP/audio/system forms, Basic auth, NVS persistence, reboot/factory reset |

Nothing here fakes a call path: the client only reports `REGISTERED`/`CONNECTED` after the PBX answers with 200 OK, and every RTP counter comes from the socket.

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
CONFIG_SIP_PHONE_WIFI_SSID="<your-ssid>"
CONFIG_SIP_PHONE_WIFI_PASSWORD="sphhospital"
```

These values are only written when no configuration exists yet; once NVS holds a configuration, changing them has no effect. `idf.py -p <PORT> erase-flash` clears NVS and re-seeds from Kconfig (it also discards SIP settings).

The repository currently ships with these lab test credentials (`<your-ssid>` / `sphhospital`) so the first boot can be verified on the test network; delete or replace them before sharing the project.

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
| Dashboard | Uptime, heap/PSRAM, Wi-Fi/IP/RSSI, setup-AP state, live SIP/call state |
| Wi-Fi | SSID/password, DHCP or static IP; saves to NVS and restarts the station |
| SIP | Server, port, username/auth ID/password, extension, transport, expiry, proxy, STUN — saved to NVS and applied on reboot |
| Phone | Speed-dial number (ANSWER button), display name, DTMF mode, auto-answer, ring duration, call timeout, plus Call/Answer/Hang up buttons |
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
| POST | `/api/config/phone` | Save speed-dial number, auto-answer, ring/call timeouts and DTMF mode |
| POST | `/api/config/admin` | Save device name/hostname and admin credentials |
| POST | `/api/system` | `{"action":"reboot"}` or `{"action":"factory_reset"}` |
| POST | `/api/call` | `{"action":"dial","number":"901"}`, `answer`, `hangup` or `reject` |

Notes:

- Every route, including the HTML/CSS/JS, returns `401` with a `WWW-Authenticate` challenge until valid credentials are supplied.
- Stored passwords are never returned by the API; leaving a password field blank in the UI keeps the existing value.
- Wi-Fi changes restart the station; if the new network cannot be joined, the `esp32SipPhone` AP appears again within ~14 s.
- New admin credentials apply on the next request; the stored hostname is used on the next boot.
- Basic auth is sent over plain HTTP, and there is no CSRF token or session timeout yet. Use a trusted LAN and change the default password.
- The HTML shell and every API route require Basic auth; `/style.css` and `/app.js` are served without auth (they contain no secrets) so the page still works when a browser does not replay credentials for subresources.

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
[BOOT] Firmware: 0.2.3
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
[BOOT] SIP/RTP/audio/web are up; telephone ready once registered.
```

> Verified on hardware (2026-11-10) with ESP-IDF 5.5.6 on COM8: firmware builds, flashes and boots with 8 MB octal PSRAM, registers to the UC120 as `901`, and completed a full incoming call with two-way G.711 RTP (see SIP status). The web UI and REST API were exercised over the LAN with `curl`.

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
- The SIP UA is written in-house because the only ESP-capable pre-compiled stack (`esp_rtc`/`esp_sip_service`) loops after the first 401 on this target. It covers the subset this phone needs: SIP over TCP, TLS/SRTP, session timers, PRACK/REFER, DNS SRV and DTMF are not implemented yet.
- Outgoing calls were verified end-to-end on 2026-11-10 with a PC UAC registered as the second extension `902` (user/pass `902`): the board dialled its speed-dial number, the PC answered, and RTP flowed both ways (board `sent 188, recv 195`; PC `tx 615, rx 586`). Dialling `901` itself reaches PBX voicemail (`183 Session Progress`).
- The lab SSID `<your-ssid>` is served by APs on two subnets (`192.168.5.x` and `192.168.7.x`). The phone roams between them and its IP changes mid-run; SIP notices within 2 s and re-registers, but HTTP clients must use the current address and the PC can only reach the `192.168.5.x` AP.
- The SPIFFS partition is reserved but unused; web assets are embedded in the app image.
- No OTA implementation despite the dual-slot partition table.

## SIP status

`sip_client` is a self-contained SIP UA on lwIP. Against the Yeastar UC120 at `192.168.5.142:5060` (extension/user/password `901`) the boot log shows:

```text
[SIP] REGISTER -> 192.168.5.142:5060 (cseq 1, expires 300)
[SIP] 401 challenge realm='UC120P-2O1L' qop=auth
[SIP] REGISTER -> 192.168.5.142:5060 (cseq 2, expires 300, auth)
[SIP] Registration Successful
[SIP] State: REGISTERED (expires 300s)
```

The UC120 challenges with `realm="UC120P-2O1L"`; some responses carry `qop="auth"` and others the legacy form without `qop`, and both are handled. Registration refreshes at 90% of the expiry. Failures back off (one retry after 60 s, then every 5 minutes) so a PBX auto-defense block is not refreshed by a retry storm. When the Wi-Fi address changes after an AP roam, the SIP task notices within 2 s, updates Via/Contact and re-registers.

Verified call (2026-11-10, COM8, board `192.168.5.139`, PC UAC `192.168.5.137`):

- incoming `INVITE` → `100 Trying` → `180 Ringing`; answer → `200 OK` with SDP (`m=audio 20000 RTP/AVP 0`)
- ACK accepted, then 241 RTP packets reached the PC in 5 s (~48/s, 20 ms pacing) with a live audio level
- hangup → `BYE` → `200 OK`; board counters `sent 80, recv 75, lost 0`, speaker stopped, state `IDLE`

The UA keeps the larger lwIP UDP pools in `sdkconfig.defaults` (64 PCBs, 64-deep receive mailbox).

## Next steps

1. **Long-call stability:** hold a 5-minute call and check RTP loss/jitter counters plus speaker/mic levels under sustained load.
2. **DTMF:** RFC 4733 `telephone-event` (payload 101) send/receive, plus a SIP INFO fallback.
3. **Transport options:** SIP over TCP, then TLS/SRTP once the certificate story is settled.
4. **Audio quality:** AEC/noise reduction (evaluate ESP-SR), proper anti-alias decimation, jitter-buffer tuning.
5. **Hardening:** system monitor task, watchdog checks, memory soak over repeated calls, OTA and config backup/restore.
6. **Release package:** wiring diagram, configuration guide, test report, binaries.
