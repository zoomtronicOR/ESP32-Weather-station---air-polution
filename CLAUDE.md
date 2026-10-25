# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This repo is at the **pre-implementation stage**. It has no firmware code yet. It contains only:

- `docs/ESP32_R201_Meteo_Stanica_Specifikacija.md`: the full functional and technical spec (in Serbian). It is the source of truth for requirements. Read the relevant section before you implement any module.
- `docs/Front.jpg` / `docs/Back.jpg`: photos of the actual dev board, used to identify the hardware.

Git remote `origin` points to the user's GitHub repo. Do not push until the user explicitly approves it (the agreed plan is to push when the project is finished).

The spec says the framework (ESP-IDF vs. Arduino-on-ESP32, with PlatformIO as the build environment) **must be confirmed with the user before implementation starts**. Do not mix APIs or libraries across frameworks or framework versions without checking compatibility.

## Language

The project language and all user-facing text (web UI, OLED, log messages shown to the user, docs) are **Serbian** (Latin script, as used in the spec). Code identifiers, MQTT topics, JSON keys and REST routes are English/snake_case, as given in the spec.

## Hardware (identified from the board photos)

- The board is a **NodeMCU ESP-32S V1.1** (38-pin) with a CP2102 USB-UART bridge and a USB-C port. It uses an ESP-32S/WROOM-class module (classic ESP32, Xtensa dual-core). It connects on **COM7**. Before you rely on the exact chip variant, flash size, or whether PSRAM is present, confirm them with `esptool.py --port COM7 chip_id` / `flash_id`.
- GPIO16/17 are broken out on the header, which suggests no PSRAM. That makes them usable for UART2 (SPS30), as the spec proposes.
- **Never use** the SD0–SD3/CLK pins (GPIO6–11, internal flash). Use strapping pins (GPIO0, 2, 5, 12, 15) only with care: GPIO12 must be low at boot. GPIO34–39 (incl. SVP/SVN) are input-only with no internal pull-ups.
- Proposed pins from the spec: I2C SDA=21, SCL=22 (shared by BME680, MPU9250/6500/9255, LTR-390 and the SSD1306 128×32 OLED); SPS30 UART2 RX=16, TX=17; WS2812B LED data=27 (driven via the RMT peripheral, not bit-banged; 5 V supply, 3.3→5 V level shifter recommended). Keep all pin assignments in one header (`include/pins.h` in the proposed layout).
- Open hardware questions (spec §22) include the exact MPU variant (MPU6500 has no magnetometer, so detect it at runtime), whether the OLED is at 0x3C or 0x3D, whether the SPS30 is on UART or I2C, and whether all modules are 3.3 V tolerant.

## Architecture (as specified)

The firmware is modular, with one manager per concern (spec §4.1 and the proposed tree in §18): `config_manager`, `wifi_manager`, `sensor_manager` plus one manager per sensor (`bme680`, `mpu`, `ltr390`, `sps30`), `display_manager`, `led_manager` (WS2812B air-quality indicator, spec §10A), `web_server`, `mqtt_manager`, `storage_manager`, `alarm_manager`, `time_manager`, `ota_manager`, `diagnostics`. Web assets (`data/`) are served from LittleFS.

Cross-cutting rules that apply to every module:

- **Standalone first.** Measuring, the OLED, the local web UI and local history must keep working without Wi-Fi, MQTT, Home Assistant or the internet.
- **Nothing blocks.** A missing sensor, Wi-Fi loss, MQTT loss or an OLED failure must not stall other modules. Use no long `delay()` calls in the main loop. Reconnects are non-blocking with backoff. Enable the watchdog. Never reboot the whole device over a single sensor error. Retry init for sensors that weren't found.
- **Every reading carries metadata:** value, unit, timestamp, sensor status, validity flag and error code. Invalid, stale or out-of-range values are never shown or stored as valid. Distinguish `unknown`, `unavailable` and a numeric value in the web UI, the API and MQTT.
- **Honest derived values.** BME680 does not measure CO₂: label IAQ/eCO₂ as derived and only expose them if a proper algorithm such as BSEC is used. Compute the LTR-390 UV index through calibration, never from raw ADC counts. Gauge color thresholds must come from cited standards or be labeled as user thresholds. The LED uses the EEA EAQI scale (worse of the PM2.5/PM10 levels) and must never show the "good" color when the PM reading is invalid or stale.
- **Flash wear.** Keep config in NVS/Preferences. Store history in LittleFS as 1–5 min aggregates (min/max/avg/count) in a ring buffer. Never write each sample to flash.
- **Time.** Store time internally as UTC and display it in `Europe/Belgrade` (ESP32 has no tz database, so use the POSIX string `CET-1CEST,M3.5.0,M10.5.0/3`). Mark time as unreliable until NTP sync succeeds (detect it with the SNTP callback). The web UI header shows device date/time with a sync-status indicator. Full requirements, statuses and `/api/v1/time` are in spec §12.
- **Secrets.** Never log passwords or MQTT credentials, and never return them from GET endpoints. Config-changing endpoints (POST/PUT) and OTA require auth. The setup AP (`METEO-ESP32-SETUP`) is password-protected.

Interfaces defined in the spec (follow them exactly):

- MQTT base topic: `meteo/esp32_r201/<metric>`. Also publish a combined JSON payload, plus `availability` (`online`/`offline`) via LWT. Home Assistant Discovery configs are retained and include `unique_id`, `device_class`, `state_class`, unit and `availability_topic`.
- REST API under `/api/v1/` (`status`, `sensors`, `readings`, `history`, `statistics`, `config`, `wifi`, `mqtt`, `alarms`, `system`). Response keys carry unit suffixes, e.g. `temperature_c` and `pm2_5_ugm3` (spec §13).
- The partition scheme must leave room for OTA partitions.
- At boot, the OLED shows connection info first: the full IP/SSID/RSSI, or the setup-AP SSID, password and IP while the portal is active. Only then does it start rotating measurement pages (spec §10.1).

## Development approach

The spec defines a staged bring-up (§19–20): first the bare ESP32, then an I2C scan, then each sensor individually, then SPS30, OLED/WS2812B LED, Wi-Fi/web, MQTT/HA, history/alarms, and finally a 24–72 h soak test. Work in this order. Each sensor should be testable on its own (the proposed `test/test_<sensor>/` directories).

## Commands

There are no build files yet. Once a PlatformIO project (`platformio.ini`) exists, the usual commands are:

```
pio run                          # build
pio run -t upload                # flash firmware (upload_port = COM7)
pio run -t uploadfs              # flash LittleFS image from data/
pio device monitor -p COM7       # serial monitor
pio test -e <env> -f test_bme680 # run a single test suite on the device
```
