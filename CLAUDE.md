# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

The project uses **PlatformIO with the Arduino framework** (pioarduino platform 53.03.13 = Arduino-ESP32 3.1.3 on ESP-IDF 5.3). The user confirmed this choice. Arduino core 3.x APIs differ from 2.x (e.g. `esp_task_wdt_config_t`, the RMT/LEDC APIs), so check that any library you add supports core 3.x.

Development follows the staged plan in spec §19–20. **Phase 1** (board bring-up: chip ID, serial, restart, watchdog) is done. Work through the later phases in order.

- `docs/ESP32_R201_Meteo_Stanica_Specifikacija.md`: the full functional and technical spec (in Serbian). It is the source of truth for requirements. Read the relevant section before you implement any module.
- `docs/Front.jpg` / `docs/Back.jpg`: photos of the actual dev board.

Git remote `origin` points to the user's GitHub repo. Do not push until the user explicitly approves it (the agreed plan is to push when the project is finished).

## Commands

```
pio run                          # build
pio run -t upload                # build + flash (COM7, set in platformio.ini)
pio run -t uploadfs              # flash LittleFS image from data/
pio device monitor               # serial monitor (115200, exception decoder)
pio test -f test_<name>          # run one on-device test suite (test/test_<name>/)
```

`pio device monitor` is interactive. To capture serial output non-interactively, open COM7 with pyserial from PlatformIO's Python (`~/.platformio/penv/Scripts/python.exe`). Set `dtr=False, rts=False` before `open()`, and pulse RTS to reset the board. The firmware prints Serbian (UTF-8) text, so call `sys.stdout.reconfigure(encoding='utf-8')`, or Windows' cp1252 console will crash on `č/ć/š`.

The firmware accepts single-character serial commands: `i` (chip info), `s` (status), `m` (all metrics), `n` (sensor status), `c` (I2C scan with chip-ID identification), `t` (I2C reliability test at 100/400 kHz), `r` (restart), `w` (deliberately hang the loop to test the watchdog), `h` (help).

## Language

The project language and all user-facing text (web UI, OLED, log messages, docs, code comments) are **Serbian** (Latin script). Code identifiers, MQTT topics, JSON keys and REST routes are English/snake_case, as given in the spec.

## Hardware (confirmed in Phase 1)

- **NodeMCU ESP-32S V1.1** (38-pin, USB-C, CP2102 on **COM7**). The chip is an **ESP32-D0WD-V3 rev v3.1**: dual core at 240 MHz, **4 MB flash, no PSRAM**. The partition table is `default.csv` (2 × 1.25 MB OTA apps + 1.375 MB LittleFS).
- With no PSRAM, GPIO16/17 are free. The SPS30 uses them as a **second, dedicated I2C bus** (`Wire1`, SDA=16, SCL=17, 100 kHz, address 0x69, SEL pin to GND, pull-ups to 3.3 V, 5 V supply). This was the user's decision, replacing UART, and keeps SPS30 traffic and faults off the main bus.
- **Never use** GPIO6–11 (internal flash). Use strapping pins (GPIO0, 2, 5, 12, 15) only with care: GPIO12 must be low at boot. GPIO34–39 are input-only with no internal pull-ups.
- All pin assignments live in `include/pins.h`: I2C SDA=21/SCL=22 (BME680, MPU9250/6500/9255, LTR-390, SSD1306 128×32 OLED), SPS30 on I2C1 SDA=16/SCL=17, WS2812B data=27, onboard blue status LED=2 (confirmed), BOOT button=0.
- WS2812B must be driven via the RMT peripheral, not bit-banged. It runs on a 5 V supply, and a 3.3→5 V level shifter is recommended.
- I2C devices confirmed in Phase 2 (`c` serial command): OLED `0x3C`, LTR-390 `0x53`, **MPU6500** `0x68` (WHO_AM_I 0x70, so no magnetometer: no compass/µT values; keep runtime WHO_AM_I detection), BME680 `0x77`. The bus is error-free at both 100 and 400 kHz (`t` command) and stays at 100 kHz for now.
- Optional sensors (SCD40/41, AHT2x, AS3935, SHT4x, SGP4x, VEML7700/BH1750, INA219, MH-Z19B, wind/rain) have reserved pins in `pins.h` and follow spec §4A. Each sensor is a driver with a common interface (`detect/begin/poll`, metrics, pin info). Identity is always confirmed via an ID register or serial number, never by address alone. UI, MQTT, HA Discovery and history are generated from the registry of active metrics. Every quantity has a user-selectable primary source with fallback. **The user's rule: never add an optional sensor whose I2C address can collide with the base devices (0x3C, 0x53, 0x68, 0x77). Do not work around a collision with address changes or a multiplexer.** That excludes DS3231, ENS160, BMP280/BME280 and TCA9548A, and also means there is no RTC.
- Still open (spec §22): the WS2812B form factor and LED count.
- The OLED is an old, dim panel on 3.3 V. It is readable and the user is keeping it. U8g2 and Adafruit SSD1306 give the same brightness, so the library isn't the cause. The firmware uses **U8g2** (fonts like `u8g2_font_6x13_te` have č ć š ž đ). Call `setBusClock(100000)` before `begin()`, or U8g2 switches the shared bus to 400 kHz. Set max brightness at init: contrast 255, pre-charge `0xF1`, VCOMH `0x40` via `sendF("ca", ...)`.
- Per-sensor on-device tests live in `test/test_bme680`, `test_mpu`, `test_ltr390` and `test_oled`. They are standalone: no `src/` code, libraries used directly. Run them with `pio test -v -f <name>`. Without `-v`, the measured values printed by the tests are hidden.

## Code layout

- `include/config.h` holds firmware constants (`FW_VERSION`, intervals, watchdog timeout).
- `include/log.h` provides the `LOG_E/W/I/D(tag, fmt, ...)` macros (levels ERROR/WARN/INFO/DEBUG, gated by `LOG_LEVEL`). Use these instead of raw `Serial.print`.
- Modules are `src/<module>.cpp/.h` pairs with a namespace-scoped API, e.g. `diagnostics::begin()`. `main.cpp` only wires modules together and runs the non-blocking loop.
- **Data flow:** `sensor_manager` owns the driver instances (`src/sensors/*_sensor.cpp`, all subclasses of `SensorDriver` in `src/sensor_driver.h`). It calls `begin()`, retries missing or failed sensors every `SENSOR_RETRY_MS`, and calls `poll(now)` each loop. Drivers are non-blocking state machines that push values into `metrics::update(MetricId, value, now)`. `metrics` validates the range and timestamps each value. Consumers (serial `m`, later OLED/web/MQTT) read the registry via `metrics::get`/`isFresh`. Never read the sensors directly. Adding a metric means adding a `MetricId` entry **and** a matching row in `DEFS` in `metrics.cpp` (a `static_assert` checks the count).
- Drivers call `markOk(now)`/`markError(code)`. `SENSOR_MAX_ERRORS` consecutive errors move a driver to `Error`, invalidate its metrics and trigger re-init. SPS30 reports `Initializing` and invalidates its metrics with `Warmup` for the first `SPS30_WARMUP_MS`.
- `i2c_bus::begin()` brings up both buses (`Wire` main, `Wire1` SPS30) and records whether external pull-ups were detected (`pullupsOk`). Adafruit libraries call `Wire.begin()` again internally. The resulting "Bus already started" warnings are harmless.
- The onboard LED blink code (spec §16.1) is chosen in `updateStatusLed()` in `main.cpp` by priority: critical (5 Hz) → sensor/wiring fault (2 flashes) → warm-up (1 Hz) → OK (1 flash / 2 s). Wi-Fi (3 flashes), MQTT (4) and portal (long flash) patterns already exist in `status_led` for later phases.
- The loop task is subscribed to the task watchdog (8 s, panic on timeout). Every new blocking path must keep calling `esp_task_wdt_reset()` or, better, never block.

## Architecture (as specified)

The firmware is modular, with one manager per concern (spec §4.1 and the proposed tree in §18): `config_manager`, `wifi_manager`, `sensor_manager` plus one manager per sensor (`bme680`, `mpu`, `ltr390`, `sps30`), `display_manager`, `led_manager` (WS2812B air-quality indicator, spec §10A), `web_server`, `mqtt_manager`, `storage_manager`, `alarm_manager`, `time_manager`, `ota_manager`, `diagnostics`. Web assets (`data/`) are served from LittleFS.

Cross-cutting rules that apply to every module:

- **Standalone first.** Measuring, the OLED, the local web UI and local history must keep working without Wi-Fi, MQTT, Home Assistant or the internet.
- **Nothing blocks.** A missing sensor, Wi-Fi loss, MQTT loss or an OLED failure must not stall other modules. Use no long `delay()` calls in the main loop. Reconnects are non-blocking with backoff. Never reboot the whole device over a single sensor error. Retry init for sensors that weren't found.
- **Every reading carries metadata:** value, unit, timestamp, sensor status, validity flag and error code. Invalid, stale or out-of-range values are never shown or stored as valid. Distinguish `unknown`, `unavailable` and a numeric value in the web UI, the API and MQTT.
- **Honest derived values.** BME680 does not measure CO₂: label IAQ/eCO₂ as derived and only expose them if a proper algorithm such as BSEC is used. Compute the LTR-390 UV index through calibration, never from raw ADC counts. Gauge color thresholds must come from cited standards or be labeled as user thresholds. The LED uses the EEA EAQI scale (worse of the PM2.5/PM10 levels) and must never show the "good" color when the PM reading is invalid or stale.
- **No microSD.** The user decided not to implement a microSD card. All history lives in LittleFS.
- **Flash wear.** Keep config in NVS/Preferences. Store history in LittleFS as 1–5 min aggregates (min/max/avg/count) in a ring buffer. Never write each sample to flash.
- **Time.** Store time internally as UTC and display it in `Europe/Belgrade` (ESP32 has no tz database, so use the POSIX string `CET-1CEST,M3.5.0,M10.5.0/3`). Mark time as unreliable until NTP sync succeeds (detect it with the SNTP callback). The web UI header shows device date/time with a sync-status indicator. Full requirements, statuses and `/api/v1/time` are in spec §12.
- **Secrets.** Never log passwords or MQTT credentials, and never return them from GET endpoints. Config-changing endpoints (POST/PUT) and OTA require auth. The setup AP (`METEO-ESP32-SETUP`) is password-protected. Credentials for local testing belong in the git-ignored `include/secrets.h`.

Interfaces defined in the spec (follow them exactly):

- MQTT base topic: `meteo/esp32_r201/<metric>`. Also publish a combined JSON payload, plus `availability` (`online`/`offline`) via LWT. Home Assistant Discovery configs are retained and include `unique_id`, `device_class`, `state_class`, unit and `availability_topic`.
- REST API under `/api/v1/` (`status`, `sensors`, `readings`, `history`, `statistics`, `config`, `wifi`, `mqtt`, `alarms`, `system`, `time`, `led`). Response keys carry unit suffixes, e.g. `temperature_c` and `pm2_5_ugm3` (spec §13).
- The web UI shows, for every sensor and peripheral, how it is wired: interface, controller pins as `GPIOxx (Pxx board label)` with each pin's role, I2C address/baud and status (spec §6.6). The firmware serves this via `/api/v1/sensors` from the same `pins.h` definitions it uses for init. Never hard-code pins in the web files.
- At boot, the OLED shows connection info first: the full IP/SSID/RSSI, or the setup-AP SSID, password and IP while the portal is active. Only then does it start rotating measurement pages (spec §10.1).

## Development approach

Follow the staged bring-up in spec §19–20: bare ESP32 (done), then an I2C scan and each sensor individually, then SPS30, OLED/WS2812B LED, Wi-Fi/web, MQTT/HA, history/alarms, and finally a 24–72 h soak test. Each sensor should be testable on its own (`test/test_<sensor>/`).

## Git

Commits are authored as the user (`Zoran Nikolić`, configured in the local repo config). Do **not** add `Co-Authored-By: Claude` or "Generated with Claude Code" lines; the user explicitly asked for this.
