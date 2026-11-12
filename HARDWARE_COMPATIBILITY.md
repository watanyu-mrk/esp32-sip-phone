# Hardware compatibility and evidence

## Board identification

Sources used, in order of authority:

1. **Guition JC4827W543 specifications (vendor PDF)** — 4.3", 480x272, ESP32-S3-WROOM-1, 4 MB flash, 8 MB PSRAM, variants N/R/C, **driver chip listed as ST3401A**, resistive or capacitive touch. <https://www.guition.com/icms/upload/fb081940d6fc11f09850077a33e1404f/FTPData/UEditor/file/2026121/1768961095791/JC4827W543%20Specifications-EN.pdf>
2. **ESPHome device page for JC4827W543C** — full working config: NV3041A over QSPI, GT911, exact pins and init sequence. <https://devices.esphome.io/devices/guition-jc4827543c>
3. **esposito BOARDS.md** — same QSPI pinout, NV3041A, backlight GPIO1. <https://github.com/ralsina/esposito/blob/main/BOARDS.md>
4. **Guition component registry board definitions** — `jc4827w543` supports GT911 / XPT2046 / no touch, SPI SD card. <https://components.espressif.com/components/guition/guition_boards>

### Controller name

The vendor calls the controller **ST3401A**; the community ports for the same board call it **NV3041A**. The register sequences in the ESPHome and Arduino/LovyanGFX ports are NV3041A sequences, and the NV3041A datasheet describes a 480x272 QSPI DBI controller (`0x02` write command, `0x32` write GRAM). Decision: implement an **NV3041A-compatible QSPI driver with the JC4827W543 vendor init sequence**, and log it as such. This is not a generic "ST3401A = NV3041A" assumption; if a unit does not initialise, the sequence array in `nv3041_panel.c` is the single place to replace.

### Touch variant

Only the **C** variant has the GT911. The R variant uses XPT2046 on SPI and the N variant has no touch. The GT911 reset strap in this firmware selects 0x5D and falls back to 0x14; a board with no GT911 logs `GT911 not responding` and the UI still renders (read-only). There is no evidence that every sub-revision uses the same touch controller, so this remains a build-time assumption, not a runtime detection.

## Memory and storage

| Item | Board | Firmware setting |
| --- | --- | --- |
| Flash | 4 MB | `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, partition table sized to 0x400000 |
| PSRAM | 8 MB octal | `CONFIG_SPIRAM_MODE_OCT`, 80 MHz |
| App partition | — | two 0x1E0000 slots, SPIFFS 0x20000 |

The copied firmware assumed an 8 MB XIAO board with 3 MB OTA slots; both were changed. The LVGL build links to 0x150610 bytes (30% of an OTA slot free), so the layout has room for the remaining phases.

## What is verified and what is not

| Item | Status |
| --- | --- |
| Project builds for esp32s3 with LVGL 9.3.0 | **Verified** (ninja, ESP-IDF 5.5.6) |
| Original SIP/RTP/web code still compiles unchanged | **Verified** by the same build |
| Display initialises and shows the UI | **Not verified** — no JC4827W543 attached to this machine |
| GT911 reports coordinates | **Not verified** — same |
| Touch orientation macros | **Not verified**; `TOUCH_MIRROR_X/Y=1` follows the ESPHome transform, adjust after the first hardware test |
| QSPI clock 20 MHz is stable | **Not verified**; reduce `LCD_QSPI_PCLK_HZ` if the panel shows corruption |
| Onboard amplifier part number and pins | **Unknown** — Phase 3 must read the schematic |
| INMP441 on 6/7/15 | Planned; pins are free on this board but not yet wired |

## Risks and mitigations

- **Wrong init sequence / controller revision.** The sequence is isolated in one table; an alternate revision can be added without touching the driver interface.
- **QSPI bandwidth.** 20 MHz quad is 10 MB/s, a 480x272 RGB565 frame is 261 KB (~26 ms). The LVGL port flushes 20-line strips synchronously, so a slow bus lowers frame rate but cannot corrupt a frame.
- **GPIO strapping.** GPIO45 (LCD CS) and GPIO3 (touch INT) are strapping pins. Neither is held at a boot-breaking level: CS idles high through the SPI peripheral, INT is driven low only briefly during GT911 reset after boot.
- **Power.** The panel and backlight draw more than the XIAO setup; use the board's USB supply, not a weak header 3.3 V feed.
