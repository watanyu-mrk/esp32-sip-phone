# knowledge.md

## Screen edition — this repository (2026-11-12)

This is the touchscreen fork of the XIAO phone: `C:\Users\Mrk\ESP32SipPhone_screen`, created from `../ESP32SipPhone` with its Git history and **no changes to the original**. The requirements document is `ipPnoneUpdatePromp.txt` (Thai). The project targets the **GUITION JC4827W543**: ESP32-S3-WROOM-1-N4R8, 4 MB flash, 8 MB octal PSRAM, 4.3" 480x272 IPS panel on **QSPI** (NV3041A protocol; Guition's PDF calls the chip ST3401A), **GT911** capacitive touch, backlight GPIO1.

Phase 1 and Phase 2 are implemented: new `display_driver`, `touch_driver`, `lvgl_port` and `ui` components; 4 MB partition table; LVGL 9.3.0 from the component registry. Phase 3 adds the JC4827W543 audio backend: `audio_i2s_input.c` (INMP441 on 6/7/15, 16 kHz -> 8 kHz) and `audio_i2s_output.c` (onboard amplifier on 42/2/41), selected by the `SIP_PHONE_AUDIO_BACKEND` Kconfig choice (default `SIP_PHONE_AUDIO_INMP441`). `ninja -C build` passes (ESP-IDF 5.5.6). **No display/touch/audio hardware test has happened yet.** The reused SIP/RTP/web/config code is unchanged and still compiles.

Hardware conflicts handled in this fork: GPIO8 is GT911 SDA but was I2S BCLK, GPIO3 is GT911 INT but was ANSWER, and GPIO41/42 are reserved for the speaker amp but were PDM mic pins. The XIAO audio backend is still compiled only if `SIP_PHONE_AUDIO_XIAO_PDM` is selected, which is wrong for this board; `CONFIG_SIP_PHONE_LEGACY_BUTTONS` defaults to `n`. The amplifier pins (42/2/41) come from the brief and are **not** confirmed by a schematic, and the INMP441 is not wired yet, so the audio path still needs bring-up.

Build recipe in this environment: `idf.py` refuses to run under Git Bash (`MSYSTEM` is present even after `unset`), so configure/build through a Python wrapper that pops `MSYSTEM` and calls CMake/Ninja directly:

```bash
export IDF_PATH=/c/esp/esp-idf
export ESP_ROM_ELF_DIR="$(cygpath -w /c/Users/Mrk/.espressif/tools/esp-rom-elfs/*)"
export PATH="/c/Users/Mrk/.espressif/python_env/idf5.5_py3.14_env/Scripts:/c/Users/Mrk/.espressif/tools/cmake/3.30.2/bin:/c/Users/Mrk/.espressif/tools/ninja/1.12.1:/c/Users/Mrk/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$PATH"
export IDF_COMPONENT_MANAGER=1
python -c "import os,subprocess,sys; os.environ.pop('MSYSTEM',None); sys.exit(subprocess.call(['ninja','-C','build']))"
```

The component manifest must live in `main/idf_component.yml`; a manifest at the project root is not scanned.

Design documents: `ORIGINAL_PROJECT_ANALYSIS.md`, `BOARD_PIN_MAPPING.md`, `HARDWARE_COMPATIBILITY.md`, `AUDIO_MIGRATION_PLAN.md`, `LVGL_ARCHITECTURE.md`, `DEVELOPMENT_ROADMAP.md`.

---

## What this repo is

**Status: working SIP phone on hardware (2026-11-10).** The ESP-IDF project boots on a XIAO ESP32-S3 Sense (COM8): 8 MB octal PSRAM, NVS configuration, Wi-Fi (`<your-ssid>`), a live PDM mic meter (~-44 dBFS ambient), the embedded web UI on port 80, and a working SIP UA. `components/sip_client` registers against a Yeastar UC120 at `192.168.5.142:5060` as extension `901` (digest with `qop=auth` or the legacy form), answers incoming INVITEs, and `rtp_audio` bridges G.711 PCMU to the I2S speaker and PDM mic using real UDP RTP (20 ms frames, 10-frame jitter buffer, send/receive counters). A call from a PC UAC verified `100/180/200 OK`, SDP, ACK, 241 RTP packets in 5 s, BYE, and board counters `sent 80, recv 75, lost 0`. Outgoing INVITEs are digest-authenticated and accepted by the UC120, and an outbound call was verified end-to-end with a PC UAC registered as second extension `902`: the board dialled the speed-dial number, the PC answered, and RTP flowed both ways (board `sent 188, recv 195`; PC `tx 615, rx 586`). Dialling `901` itself reaches PBX voicemail (`183`). The Phone tab speed-dial number is dialled by the ANSWER button via `phone.default_number`. The pre-compiled `esp_sip_service`/`esp_rtc` stack was removed because it re-read a stale 262-byte response forever after the first REGISTER, never sent the authenticated retry and tripped the task watchdog. The lab SSID is served by APs on both `192.168.5.x` and `192.168.7.x`; the phone roams, so SIP re-registers when its address changes (checked every 2 s).

The hardware buttons work end-to-end: ANSWER (GPIO3) answers a ringing call or dials `phone.default_number` when idle, HANGUP (GPIO2) ends or cancels. They first crashed the board with `***ERROR*** A stack overflow in task buttons has been detected.`, because the callback ran on a 3 KB `buttons` task while `sip_client_dial/answer/hangup` build SDP and SIP messages in ~2 KB of locals; the task stack is now 8 KB and three real presses (dial-out with `180 Ringing` → `200 OK` → RTP `sent 129, recv 79, lost 0` → BYE, answer an incoming INVITE → RTP `sent 171, recv 118, lost 0` → BYE, and another dial-out) completed without a panic or reboot. Fix committed as `d6f2ac5`.

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

## Layout (keep modules separated)

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

A normal Windows CMD/PowerShell terminal does not have this problem. If even the in-process `MSYSTEM` pop does not work (observed 2026-11-10: `idf.py` printed the warning and exited 0 without building, so check the log line count before believing it), skip `idf.py` entirely — the CMake cache already exists:

```bash
export IDF_PATH=/c/esp/esp-idf
export PATH="/c/Users/Mrk/.espressif/python_env/idf5.5_py3.14_env/Scripts:/c/Users/Mrk/.espressif/tools/cmake/3.30.2/bin:/c/Users/Mrk/.espressif/tools/ninja/1.12.1:/c/Users/Mrk/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$PATH"
ninja -C build                                  # builds build/esp32_sip_phone.bin
cd build && python -m esptool --chip esp32s3 -p COM8 -b 460800 \
  --before default_reset --after hard_reset write_flash @flash_args
```

`flash_args` paths are relative to `build/`, so run esptool from there.

## Current status (2026-11-10)

| Item | State |
| --- | --- |
| HEAD | `d6f2ac5` "Give the button task enough stack for SIP message building" |
| Remote | `origin` https://github.com/watanyu-mrk/esp32-sip-phone (private) — push of `d6f2ac5` still pending, needs a PAT |
| Firmware on the board | 0.2.3 with the 8 KB `buttons` task, flashed to COM8 and registered (`REGISTERED, expires 300s`) |
| Verified on hardware | REGISTER with digest; incoming and outgoing calls; two-way G.711 (PCMU and PCMA); ANSWER dial-out, ANSWER on ring, remote BYE / local teardown; no stack overflow on any button press |
| Not verified yet | HANGUP while an outgoing INVITE is still pending (the path that crashed before the stack fix); DTMF; TCP/TLS |
| Environment notes | `idf.py` is unusable from this Git Bash (MSys check) — use `ninja -C build`; the board currently gets `192.168.7.201`, which this PC cannot reach, so testing is serial-only; serial watcher pattern writes `.answer_watch.log` (gitignored) |
| Credentials | extension `901`/`901` against UC120 `192.168.5.142:5060`, web admin `admin`/`admin`; lab Wi-Fi is kept out of git |

## Conventions and constraints

- **NVS** stores all configuration; changing IP/Wi-Fi must not lose settings. Factory reset clears it.
- **First-boot Wi-Fi:** `CONFIG_SIP_PHONE_WIFI_SSID` / `CONFIG_SIP_PHONE_WIFI_PASSWORD` (Kconfig menu "XIAO SIP Phone") seed NVS only while it is empty; after that NVS wins. The driver uses `WIFI_STORAGE_RAM`, so `config_manager` remains the single source of credentials. `sdkconfig.defaults` currently seeds lab test credentials `<your-ssid>` / `sphhospital`; remove them before sharing the repo.
- **Setup AP:** `WIFI_FALLBACK_AP_SSID` = `esp32SipPhone`, open, 192.168.4.1. Started by `wifi_manager_start_fallback_ap()` when there is no SSID, no IP within 10 s, or 4 consecutive reconnect failures. It restarts the Wi-Fi driver, so never raise it during a call. `wifi_manager_stop_fallback_ap()` returns to STA-only.
- **Audio:** PDM RX at 16 kHz is decimated 2:1 to 8 kHz; playback is 8 kHz mono 16-bit. Both directions use FreeRTOS stream buffers and bounded I2S waits in dedicated tasks (`audio_priv.h` is internal, `audio_driver.h` is the API). Capture starts at boot for metering; playback starts with a call. Never start the SD SPI driver — GPIO7/8/9 are shared.
- **SIP:** in-house UA in `components/sip_client/sip_client.c` on lwIP (no managed component). REGISTER and INVITE with MD5 digest (`qop=auth` and RFC 2069; INVITE keeps its own nonce and retries with a new CSeq/branch, ACK for the 401 uses the challenged CSeq and ACK for a 2xx keeps the INVITE CSeq), refresh at 90% of the expiry, 60 s then 5 min failure backoff, INVITE/ACK/CANCEL/BYE/OPTIONS, SDP offer/answer for PCMU (0) and PCMA (8), and re-registration when the local IP changes. Responses go to the request source, not the registrar, so direct peer calls work; in-dialog requests follow the call peer. Not implemented: TCP/TLS, SRTP, DTMF, PRACK/REFER, session timers.
- **Web UI:** ESP-IDF `httpd` on port 80, HTML/CSS/JS embedded from `web/` via `EMBED_TXTFILES`. The HTML shell and every `/api` route require HTTP Basic auth against `system.web_user`/`web_password` with a `WWW-Authenticate` challenge; `/style.css` and `/app.js` are public (no secrets) and served `no-store` so the UI still boots when a browser does not replay credentials for subresources. Passwords are never serialised, only `password_set` flags. Routes: `GET /api/status`, `GET /api/config`, `POST /api/config/{wifi,audio,sip,phone,admin}`, `POST /api/system` (`reboot`, `factory_reset`), `POST /api/call` (`dial`, `answer`, `hangup`, `reject`). The Phone tab stores `phone.default_number`, which the ANSWER button dials when the phone is idle (phone_control.c), plus auto-answer/ring/call-timeout and DTMF mode. Wi-Fi saves restart the station; audio changes apply immediately; admin credentials apply to the next request. Factory reset restores defaults and reboots.
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
