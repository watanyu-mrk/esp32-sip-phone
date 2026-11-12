# LVGL architecture

## Components

```text
display_driver   QSPI bus + NV3041A panel + LEDC backlight
touch_driver     GT911 over I2C, polling reads, coordinate mapping
lvgl_port        lv_init, display, draw buffers, flush, indev, tick, task, mutex
ui               screens and widgets (home screen in Phase 2)
```

Dependency direction: `ui -> lvgl_port -> {display_driver, touch_driver}`. `main` requires all four and starts them in that order. SIP, RTP, audio, Wi-Fi and web components do not include LVGL headers.

## Rendering path

```text
LVGL render (LVGL task)
  -> flush_cb()
     -> esp_lcd_panel_draw_bitmap()   (QSPI, DMA)
     -> wait for on_color_trans_done  (binary semaphore, 200 ms cap)
     -> lv_display_flush_ready()
```

- Two partial buffers, 480 × 20 px × 2 B = 19,200 B each, allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`. PSRAM is deliberately not used for draw buffers: the QSPI DMA path is faster from internal RAM.
- RGB565 (`LV_COLOR_DEPTH_16`), partial rendering, 30 ms default refresh.
- Flush is synchronous per strip. It costs frame rate on a slow bus but guarantees the buffer is not reused mid-transfer and keeps the logic simple enough to audit.

## Tasks and priorities

| Task | Priority | Stack | Role |
| --- | --- | --- | --- |
| Audio capture/playback | above LVGL (existing driver, currently disabled) | existing | 20 ms RTP frames must never wait on rendering |
| SIP / RTP / Wi-Fi | existing | existing | network and signalling |
| `lvgl` | 3 (low) | 8 KB | `lv_timer_handler()` every 5–20 ms |
| `buttons` | 5 (existing, disabled) | 8 KB | not used on this board |

The LVGL task is the only caller of LVGL APIs. Anything outside it must take `lvgl_port_lock()` first; `ui_init()` is called from `app_main` under that lock. SIP/RTP code has no LVGL dependency, so a render burst cannot delay an RTP packet beyond its task priority.

## Tick

`esp_timer` periodic callback at 1 kHz calls `lv_tick_inc(1)`. LVGL does not run its own tick thread (`LV_USE_OS` stays off); scheduling is the port's responsibility.

## Input

`lv_indev` read callback polls the GT911 (register 0x814E). Coordinates are transformed once in `touch_driver_read()` by `TOUCH_SWAP_XY` / `TOUCH_MIRROR_X` / `TOUCH_MIRROR_Y`. LVGL delivers press/release/click/long-press; the 30 px bottom bar and 150 × 38 px dial keys are sized for a fingertip on the 4.3" panel.

## UI state flow

The Phase 2 home screen polls once every 500 ms:

- `config_manager_get()` for device name and extension,
- `wifi_manager_is_connected()` / `wifi_manager_get_rssi()`,
- `sip_client_get_reg_state()` and `sip_client_get_call_state()` for the header and call line,
- `phone_control_dial()` is the only call-control entry point; the UI never builds SIP messages.

Phase 5/6 replaces polling with an event queue from `phone_control` (the brief's `phone_event_t`) so the UI reacts to registration, ringing and hangup without querying the stack. Caller ID and call duration will come from that event payload, not from SIP parsing in the UI.

## Memory budget

| Item | Bytes |
| --- | --- |
| LVGL heap pool | 48 KB |
| Two draw buffers | 38.4 KB internal DMA |
| LVGL task stack | 8 KB |
| App image (current) | ~1.3 MB of a 1.875 MB slot |

PSRAM is reserved for future surfaces/contact lists, not for DMA buffers. The web UI assets remain embedded in the app image (no SPIFFS dependency).

## Fonts

Montserrat 14/16/20/28 are enabled through Kconfig. Thai text is **not** covered by the built-in fonts yet: Phase 5 will convert a Thai TTF with `lv_font_conv` into `assets/fonts/` and register it in the UI. Until then the UI uses English labels only.
