# Board pin mapping — GUITION JC4827W543

The board is an ESP32-S3-WROOM-1-N4R8 module (4 MB flash, 8 MB octal PSRAM) on a 4.3" 480x272 IPS panel. Guition sells it in three variants: **N** (no touch), **R** (resistive, XPT2046) and **C** (capacitive, GT911). This firmware targets the **C** variant and treats the display/touch pins as build configuration.

## Display (QSPI, implemented)

| Signal | GPIO | Notes |
| --- | --- | --- |
| LCD CS | 45 | strapping pin, driven by SPI peripheral |
| LCD SCK | 47 | QSPI clock |
| LCD D0 | 21 | quad data 0 |
| LCD D1 | 48 | quad data 1 |
| LCD D2 | 40 | quad data 2 |
| LCD D3 | 39 | quad data 3 |
| Backlight | 1 | active high, LEDC PWM at 5 kHz |
| LCD reset | — | not wired; controller reset is command 0x01 |

Defines: `components/display_driver/display_driver.h`.

## Touch (GT911, I2C, implemented)

| Signal | GPIO | Notes |
| --- | --- | --- |
| SDA | 8 | 400 kHz, internal pull-up enabled |
| SCL | 4 | 400 kHz, internal pull-up enabled |
| INT | 3 | strapping/JTAG pin; used only for the reset address strap, then input |
| RST | 38 | active low |

Address 0x5D (INT low during reset release) with 0x14 as fallback. Defines: `components/touch_driver/touch_driver.h`.

## Expansion headers (from the brief — verify against the silkscreen)

```text
Header 1: GPIO46, GPIO9, GPIO14, GPIO5
Header 2: GPIO6,  GPIO7, GPIO15, GPIO16
Header 3: GND, 3.3V, GPIO17, GPIO18
Header 4: GND, 3.3V, GPIO17, GPIO18   <- duplicate of header 3
```

GPIO46 is input-only and a strapping pin. Header 3/4 are documented as duplicates; do not assume two independent pairs until the schematic confirms it.

## Planned audio wiring (Phase 3, not yet implemented)

| Function | Signal | GPIO | Status |
| --- | --- | --- | --- |
| INMP441 | SCK/BCLK | 6 | free on this board, no conflict |
| INMP441 | WS/LRC | 7 | free on this board, no conflict |
| INMP441 | SD | 15 | free on this board, no conflict |
| INMP441 | L/R | GND | selects left channel |
| Speaker amp | BCLK | 42 | proposed in the brief; conflicts with the legacy PDM clock until Phase 3 |
| Speaker amp | LRC | 2 | proposed in the brief; free |
| Speaker amp | DOUT | 41 | proposed in the brief; conflicts with the legacy PDM data until Phase 3 |

The brief's speaker pins are **unverified** for this board revision: the onboard amplifier (NS4168 or equivalent) is not documented in the material found. Phase 3 must confirm it from the schematic before driving those pins.

## Conflict matrix

| GPIO | New use | Legacy use in the copied firmware | Resolution |
| --- | --- | --- | --- |
| 3 | GT911 INT | ANSWER button | buttons disabled (`CONFIG_SIP_PHONE_LEGACY_BUTTONS=n`) |
| 8 | GT911 SDA | I2S BCLK | legacy audio disabled (`CONFIG_SIP_PHONE_LEGACY_XIAO_AUDIO=n`) |
| 41 | planned speaker DOUT | PDM DATA | same |
| 42 | planned speaker BCLK | PDM CLK | same |
| 7 | planned INMP441 BCLK | I2S DOUT | same |
| 9 | free | I2S LRC | same |

No display or touch pin overlaps another display/touch pin. GPIO45 is a strapping pin but is only driven as chip select after boot; GPIO3 is read as an input after the GT911 reset sequence.
