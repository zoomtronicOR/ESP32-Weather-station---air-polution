// ESP32 R201 -- Pametna meteo stanica
// Glavni program: pokretanje modula i neblokirajuća glavna petlja

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "air_quality.h"
#include "config.h"
#include "config_manager.h"
#include "diagnostics.h"
#include "display_manager.h"
#include "i2c_bus.h"
#include "log.h"
#include "metrics.h"
#include "pins.h"
#include "sensor_manager.h"
#include "status_led.h"
#include "time_manager.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static uint32_t s_lastStatusMs = 0;
static uint32_t s_lastMetricsPrintMs = 0;
static uint32_t s_lastConnInfoMs = 0;

static bool s_bootButtonDown = false;
static uint32_t s_bootButtonChangeMs = 0;
static bool s_bootLongHandled = false;

// Taster BOOT (GPIO0): kratak pritisak ponovo prikazuje podatke za konekciju na OLED-u,
// dug pritisak (5 s) ručno pokreće konfiguracioni portal (specifikacija §7.3, §10.1)
static void handleBootButton(uint32_t now) {
    bool down = digitalRead(PIN_BOOT_BUTTON) == LOW;
    if (down != s_bootButtonDown && now - s_bootButtonChangeMs >= 50) {
        uint32_t heldMs = now - s_bootButtonChangeMs;
        s_bootButtonDown = down;
        s_bootButtonChangeMs = now;
        if (down) {
            s_bootLongHandled = false;
        } else if (!s_bootLongHandled && heldMs < 1000) {
            display_manager::showConnection(now);
        }
    }
    if (s_bootButtonDown && !s_bootLongHandled && now - s_bootButtonChangeMs >= WIFI_PORTAL_BUTTON_MS) {
        s_bootLongHandled = true;
        LOG_I(TAG, "Taster BOOT držan 5 s -- pokreće se konfiguracioni portal");
        wifi_manager::startPortal(now);
        display_manager::showConnection(now);
    }
}

// Izbor obrasca statusne LED diode; važniji problem ima prednost (specifikacija §16.1)
static void updateStatusLed(uint32_t now) {
    using status_led::Pattern;
    sensor_manager::Health h = sensor_manager::health();

    Pattern pattern;
    bool noSensorWorks = h.total > 0 && h.ok == 0 && h.initializing == 0;
    if (!i2c_bus::ready(Wire) || noSensorWorks || ESP.getFreeHeap() < LOW_HEAP_BYTES) {
        pattern = Pattern::Critical;
    } else if (wifi_manager::portalActive()) {
        pattern = Pattern::Portal;
    } else if (h.faulty > 0 || !i2c_bus::pullupsOk(Wire) || !i2c_bus::pullupsOk(Wire1)) {
        pattern = Pattern::SensorFault;
    } else if (!wifi_manager::connected()) {
        pattern = Pattern::NoWifi;
    } else if (h.initializing > 0) {
        pattern = Pattern::Warmup;
    } else {
        pattern = Pattern::Heartbeat;
    }

    status_led::set(pattern);
    status_led::update(now);
}

static void setupWatchdog() {
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    // Arduino jezgro već inicijalizuje task watchdog, pa se samo menja konfiguracija
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    if (err == ESP_OK) {
        err = esp_task_wdt_add(NULL);
    }
    if (err == ESP_OK) {
        LOG_I(TAG, "Watchdog uključen, timeout %u ms", WDT_TIMEOUT_MS);
    } else {
        LOG_E(TAG, "Watchdog nije uključen (greška %d)", err);
    }
}

static void printHelp() {
    Serial.println();
    Serial.println("Komande (poslati jedan znak preko serijskog porta):");
    Serial.println("  i -- podaci o uređaju");
    Serial.println("  s -- trenutni status");
    Serial.println("  m -- trenutna merenja");
    Serial.println("  n -- status senzora");
    Serial.println("  c -- I2C skeniranje (obe magistrale)");
    Serial.println("  t -- test pouzdanosti I2C magistrale (100 i 400 kHz)");
    Serial.println("  p -- pokretanje konfiguracionog portala");
    Serial.println("  r -- softverski restart");
    Serial.println("  w -- test watchdog-a (glavna petlja se namerno blokira)");
    Serial.println("  h -- pomoć");
    Serial.println();
}

static void handleSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case 'i': diagnostics::printChipInfo(); break;
            case 's': diagnostics::printStatus(); break;
            case 'm': metrics::printAll(millis()); break;
            case 'n': sensor_manager::printStatus(millis()); break;
            case 'c': i2c_bus::printScan(); break;
            case 't': i2c_bus::stressTest(2000); break;
            case 'p': wifi_manager::startPortal(millis()); break;
            case 'h': printHelp(); break;
            case 'r':
                LOG_W(TAG, "Softverski restart na zahtev korisnika");
                Serial.flush();
                ESP.restart();
                break;
            case 'w':
                LOG_W(TAG, "Test watchdog-a: petlja se blokira, restart za oko %u s", WDT_TIMEOUT_MS / 1000);
                Serial.flush();
                while (true) {
                    delay(100);  // watchdog se ne resetuje
                }
            default: break;
        }
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);

    status_led::begin();  // stalno upaljena dok traje pokretanje

    LOG_I(TAG, "ESP32 meteo stanica, firmware %s", FW_VERSION);
    diagnostics::begin();
    diagnostics::printChipInfo();
    setupWatchdog();
    config_manager::begin();
    time_manager::begin();
    i2c_bus::begin();
    i2c_bus::printScan();
    display_manager::begin(millis());  // uvodni ekran, pa podaci za konekciju
    sensor_manager::begin();
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    wifi_manager::begin(millis());
    web_server::begin();  // posle Wi-Fi-ja, jer mrežni sloj mora biti pokrenut
    printHelp();
}

void loop() {
    esp_task_wdt_reset();

    uint32_t now = millis();

    sensor_manager::poll(now);
    air_quality::update(now);

    wifi_manager::update(now);
    time_manager::update(now, wifi_manager::connected());
    web_server::update(now);

    if (now - s_lastConnInfoMs >= 500) {
        s_lastConnInfoMs = now;
        display_manager::setConnectionInfo(wifi_manager::connectionInfo(), now);
    }
    display_manager::update(now);
    handleBootButton(now);
    updateStatusLed(now);

    if (now - s_lastStatusMs >= STATUS_INTERVAL_MS) {
        s_lastStatusMs = now;
        diagnostics::printStatus();
    }

    if (now - s_lastMetricsPrintMs >= METRICS_PRINT_MS) {
        s_lastMetricsPrintMs = now;
        metrics::printAll(now);
    }

    handleSerial();
    delay(10);
}
