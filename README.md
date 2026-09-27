# e32l7wdisplay - 7" ESP32-S3 Temperature Display Dashboard

WiFi-connected weather/sensor dashboard on a **7" 800×480 RGB display (Waveshare ESP32-S3-Touch-LCD-7)**. It collects temperature, humidity, battery and signal data from **passive BLE** sensors, **Home Assistant** (WebSocket) or **Shelly MQTT** (Gen1 + Gen2/Gen3) sources, renders them on a configurable LVGL grid, and exposes itself to Home Assistant via **MQTT autodiscovery** (display switch, uptime, MCU temperature).

> Built with [OpenCode](https://opencode.ai) CLI.

## Hardware

Board: **Waveshare ESP32-S3-LCD-7** — ESP32-S3 @ 240 MHz, 8 MB flash (DIO), octal PSRAM @ 80 MHz, 800×480 RGB565 LCD, CH422G I/O expander. (Touch not used)

| Function | Wiring |
| --- | --- |
| LCD backlight enable | CH422G EXIO3 (on/off only, no PWM) |
| LCD reset | CH422G EXIO4 |
| SD card CS | CH422G EXIO5 (no SD code in firmware) |
| CH422G control | I2C, SDA GPIO8 / SCL GPIO9 |

## Features

- **Display**: LVGL 9.5 dashboard grid (1–3 rows, 1–4 cols, max 12 cells), per-cell temperature/humidity/battery/signal rows, stale highlighting (>1h red), top status bar (clock via NTP + timezone setting, STA/AP IP, WiFi RSSI fan with 5 levels), SRAM draw buffers (LVGL heap internal by design).
- **Data sources**: passive BLE (NimBLE observer: ATC/Xiaomi/BTHome), Home Assistant WebSocket (entity registry + states + live `state_changed`, device-grouped rows), Shelly MQTT (Gen1 `shellies/` + Gen2/Gen3 NotifyStatus/status, online-triggered `GetConfig` discovery).
- **HA integration** (optional MQTT client): display on/off switch, uptime + MCU temperature sensors, all with autodiscovery under `homeassistant/<component>/<dev-name>/…`; shares the Shelly connection when broker credentials match an active Shelly source, else runs its own (LWT availability included).
- **WebUI**: Info (CPU/RAM/PSRAM/flash/partitions/uptime/clock/MCU temp), WiFi, Integration, Data Source (incl. manual *Start discovering*), Display, Dashboard (12-row assignment, survives grid resize), Settings (NTP + POSIX TZ + credentials), Firmware, Reboot. Plus a 1:1 `/dashboard` mirror page.
- **Safeboot recovery**: minimal stub on the factory partition (no auth, open `L7-RECOVERY` AP) — `/up` file upload writes app0, `/wi` STA setup, automatic bootloader fallback on bad image.
- **Robustness**: 30 s task watchdog (main task), bounded LVGL/sensor mutex waits with fail-open, LVGL asserts abort with backtrace instead of spinning, static UbuntuMono bitmap fonts (no runtime TTF).

## Partition layout (`partitions.csv`, 8 MB flash)

| Offset | Size | Content |
| --- | --- | --- |
| 0x9000 | 20 KiB | NVS (WiFi data) |
| 0xe000 | 8 KiB | otadata (boots app0) |
| 0x10000 | 832 KiB | safeboot recovery (factory) |
| 0xE0000 | ~2.9 MB | app0 (this firmware, OTA target) |
| 0x3B0000 | ~4.5 MB | LittleFS spiffs (`/spiffs/config.json`) |

## Dependencies (managed_components, auto-fetched)

| Component | License |
| --- | --- |
| [lvgl/lvgl](https://github.com/lvgl/lvgl) v9 | MIT |
| [espressif/esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port) | Apache-2.0 |
| [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) (board support) | Apache-2.0 |
| [espressif/esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client) | Apache-2.0 |
| [joltwallet/esp_littlefs](https://github.com/joltwallet/esp_littlefs) | MIT |

## Build & Flash

### Prerequisites

- [PlatformIO](https://platformio.org/) (Install: `pip install platformio`)
- ESP32-S3 toolchain/framework (downloaded automatically by PlatformIO)

### Build

```
pio run -e esp32-s3     # application -> dist/ota-esp32s3-8mb.bin
pio run -e safeboot     # recovery stub (factory partition)
python3 tools/make_factory.py   # dist/factory-8mb.img (needs both built)
```

Per-env sdkconfig: `sdkconfig.defaults` + `sdkconfig.defaults.safeboot` (wired via `-DSDKCONFIG_DEFAULTS`, needs `pio run -e safeboot -t clean` after editing). Fonts regenerate with `tools/make_fonts.sh` (needs `lv_font_conv`); icons are procedural LVGL primitives (no bitmap assets).

### Flash

- **OTA update** (normal path): WebUI → Firmware → upload `dist/ota-esp32s3-8mb.bin` (safeboot writes app0 transparently).
- **USB recovery / first flash**: hold BOOT, tap RESET, then
```
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write_flash 0x0 dist/factory-8mb.img
```
(flash mode DIO, 8 MB). Serial monitor: `pio device monitor -b 115200`.
