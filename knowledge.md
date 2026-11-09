# knowledge.md

## What this repo is

**Status: configuration, Wi-Fi, audio capture and the web UI work on hardware; telephony does not.** The ESP-IDF project tree boots on a XIAO ESP32-S3 Sense (verified 2026-11-09, COM8): 8 MB octal PSRAM detected, configuration defaults written to NVS, Wi-Fi joined (`spd_wifi`, IP 192.168.1.171), PDM microphone producing live dBFS meter lines (~-44 dBFS ambient), and an embedded web UI on port 80. HTTP was verified from the host with `curl`: unauthenticated and wrong-password requests return 401, `/` serves the UI, `/api/status` reports live IP/RSSI/heap, and `POST /api/config/audio` changes a value that `GET /api/config` reads back. SIP, RTP and buttons are still explicit stubs returning `ESP_ERR_NOT_SUPPORTED`; no calls are placed. The I2S speaker path initialises but has not been exercised with audio. When no SSID is stored, boot gets no IP within 10 s, or four reconnects fail, the Wi-Fi manager raises an open setup AP `esp32SipPhone` at 192.168.4.1 (`WIFI_MODE_APSTA`, STA retries continue), which now serves the same UI.

The requirements document is `promp.txt` (Thai): the authoritative brief for an **ESP32-S3 SIP IP phone** on a **Seeed Studio XIAO ESP32-S3 Sense**, built with **ESP-IDF in C/C++**. It must work as a real phone against FreePBX/Asterisk: SIP REGISTER, incoming/outgoing calls, RTP two-way audio, all configured through an on-device web UI.

Treat `promp.txt` as the requirements document — read it before implementing anything. The agent role defined there is a Senior Embedded Systems Engineer; working rules that matter most:

- Build every phase before moving on; no mock/simulated SIP or RTP presented as working.
- Never claim hardware testing that did not happen — say explicitly what was verified and how.
- No blocking audio code; release resources on every call teardown.
- Don't log SIP or web passwords.

## Hardware map (fixed by the spec)

| Function | Signal | GPIO |
| --- | --- | --- |
| I2S output | DOUT | 7 |
| I2S output | BCLK | 8 |
| I2S output | LRC | 9 |
| PDM mic | DATA | 41 |
| PDM mic | CLK | 42 |
| Button | ANSWER | 3 |
| Button | HANGUP | 2 |

Known gotchas the spec calls out:

- **GPIO3 is a strapping pin** — button wiring must not prevent boot if held.
- GPIO7/8/9 are shared with the microSD circuit — **do not start the SD SPI driver**; it would disturb I2S.
- Buttons need debounce (long press optional) and pull-up/pull-down that matches the real circuit.
- Verify PSRAM presence and enable it per the actual board.

## Audio requirements

- Codecs: **G.711 PCMU/PCMA only** (primary), via SDP negotiation; 8000 Hz, mono, 16-bit PCM, 20 ms packetization, RTP payload type from negotiation.
- Path in: PDM → I2S PDM RX → PCM → processing (gain, NR, optional AEC) → RTP encode → UDP.
- Path out: RTP UDP → jitter buffer → decode → I2S TX → speaker.
- Needs jitter buffer, packet-loss handling, mic gain, output volume, NR; AEC only if resources/libs (ESP-SR) actually support it.
- Echo/AEC and full-duplex behaviour must be analysed, not assumed.

## Layout (scaffolded; keep modules separated)

```
esp32_sip_phone/
├── CMakeLists.txt, sdkconfig.defaults, partitions.csv
├── main/            main.c, app_config.h, app_state.h
├── components/      sip_client/, rtp_audio/, audio_driver/,
│                    phone_control/, wifi_manager/, web_server/,
│                    config_manager/
├── web/             index.html, style.css, app.js
└── README.md
```

FreeRTOS tasks: SIP, RTP receive, audio capture, audio playback, buttons, web server, Wi-Fi events, system monitor. Audio tasks must never be blocked by SIP/network work; define clear ownership of audio buffers and the SIP session (queues, event groups, mutexes, ring buffers).

## Build / flash commands

Verified in this environment:

| Item | Value |
| --- | --- |
| ESP-IDF | 5.5.6 at `C:\esp\esp-idf` |
| Python venv | `C:\Users\Mrk\.espressif\python_env\idf5.5_py3.14_env` |
| Toolchain | `xtensa-esp-elf` 14.2.0 in `C:\Users\Mrk\.espressif\tools` |
| Serial port | **COM8** |

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM8 flash
```

`idf.py build` succeeds (app image 0xDE4F0 bytes, 71% of the OTA slot free) and `flash` writes and verifies bootloader, partition table and app. The board boots with 8 MB octal PSRAM (`esp_psram: Found 8MB PSRAM device`) and a live mic meter.

### Git Bash caveat (MSYS)

This IDF install refuses to run under Git Bash: `install.sh`, `install.bat`, `export.sh` and every `idf_tools.py` call abort with "MSys/Mingw is not supported" while `MSYSTEM` is set, and `env -u MSYSTEM` does **not** clear it here. Working recipe: put the venv scripts and toolchain dirs on `PATH`, set `IDF_PATH` and `ESP_ROM_ELF_DIR`, then run `idf.py` with the venv Python after popping `MSYSTEM` in-process (children inherit the cleaned env):

```bash
IDF=/c/esp/esp-idf
VENV=/c/Users/Mrk/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe
export IDF_PATH=$IDF
export ESP_ROM_ELF_DIR="$(cygpath -w /c/Users/Mrk/.espressif/tools/esp-rom-elfs/*)"
export PATH="$(dirname "$VENV"):/c/Users/Mrk/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin:/c/Users/Mrk/.espressif/tools/cmake/*/bin:/c/Users/Mrk/.espressif/tools/ninja/*:$PATH"
"$VENV" - build <<'PY'
import os, runpy, sys
os.environ.pop('MSYSTEM', None)
os.environ['IDF_PATH'] = r'C:\esp\esp-idf'
sys.path.insert(0, r'C:\esp\esp-idf\tools')
sys.argv = [r'C:\esp\esp-idf\tools\idf.py'] + sys.argv[1:]
runpy.run_path(r'C:\esp\esp-idf\tools\idf.py', run_name='__main__')
PY
```

A normal Windows CMD/PowerShell terminal does not have this problem.

## Conventions and constraints

- **NVS** stores all configuration; changing IP/Wi-Fi must not lose settings. Factory reset clears it.
- **First-boot Wi-Fi:** `CONFIG_SIP_PHONE_WIFI_SSID` / `CONFIG_SIP_PHONE_WIFI_PASSWORD` (Kconfig menu "XIAO SIP Phone") seed NVS only while it is empty; after that NVS wins. The driver uses `WIFI_STORAGE_RAM`, so `config_manager` remains the single source of credentials. `sdkconfig.defaults` currently seeds lab test credentials `spd_wifi` / `sphhospital`; remove them before sharing the repo.
- **Setup AP:** `WIFI_FALLBACK_AP_SSID` = `esp32SipPhone`, open, 192.168.4.1. Started by `wifi_manager_start_fallback_ap()` when there is no SSID, no IP within 10 s, or 4 consecutive reconnect failures. It restarts the Wi-Fi driver, so never raise it during a call. `wifi_manager_stop_fallback_ap()` returns to STA-only.
- **Audio:** PDM RX at 16 kHz is decimated 2:1 to 8 kHz; playback is 8 kHz mono 16-bit. Both directions use FreeRTOS stream buffers and bounded I2S waits in dedicated tasks (`audio_priv.h` is internal, `audio_driver.h` is the API). Capture starts at boot for metering; playback starts with a call. Never start the SD SPI driver — GPIO7/8/9 are shared.
- **Web UI:** ESP-IDF `httpd` on port 80, HTML/CSS/JS embedded from `web/` via `EMBED_TXTFILES`. Every route (static and `/api`) requires HTTP Basic auth against `system.web_user`/`web_password` with a `WWW-Authenticate` challenge; passwords are never serialised, only `password_set` flags. Routes: `GET /api/status`, `GET /api/config`, `POST /api/config/{wifi,audio,sip,admin}`, `POST /api/system` (`reboot`, `factory_reset`). Wi-Fi saves restart the station; audio changes apply immediately; admin credentials apply to the next request. Factory reset restores defaults and reboots.
- Logging: `ESP_LOGI/W/E/D` with module tags `WIFI`, `SIP`, `RTP`, `AUDIO`, `BUTTON`, `WEB`, `SYSTEM`, `MEMORY`; log SIP messages, SDP negotiation, RTP stats, buffer under/overrun, memory, watchdog.
- SIP stack choice is an open decision — validate ESP32-S3/ESP-IDF compatibility (e.g. PJSIP or an ESP-capable stack) before committing; do not hand-roll a fake stack. UDP transport default, TCP optional, RFC 3261 digest auth, RFC 2833/4733 DTMF, keepalive, session timers.
- Defaults in the brief (192.168.4.29, ext 101, port 5060, UDP) are test values only — everything must be configurable.
- Hardware pin defines live in the owning component headers (`audio_driver.h` for I2S/PDM, `button.h` for buttons, `rtp_client.h` for RTP constants) because ESP-IDF components cannot include `main/app_config.h`. That header holds only firmware identity and shared constants.

Keep RAM/PSRAM/CPU limits in mind; PSRAM is limited and audio buffers are the main consumer.

## Development order (spec §10)

1. Hardware validation (pins, PDM, I2S, buttons, PSRAM, Wi-Fi)
2. Audio standalone (levels over serial, PCM conversion, playback, full duplex)
3. SIP signaling
4. RTP + jitter buffer + DTMF
5. SIP + RTP integration (two-way audio)
6. Web UI
7. System integration (auto-register, reconnect, reboot, factory reset, memory stability)
8. Release package (source, buildable project, binaries, wiring diagram, README, config guide, test report, known issues, troubleshooting)

Deliverables must include real source and config files — never pseudocode alone.
