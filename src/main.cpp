// ESP32 R201 -- Pametna meteo stanica
// Osnovni firmware: dijagnostika, watchdog, I2C magistrale i modularno prikupljanje merenja

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "diagnostics.h"
#include "i2c_bus.h"
#include "log.h"
#include "metrics.h"
#include "sensor_manager.h"
#include "status_led.h"

static const char *TAG = "main";

static uint32_t s_lastStatusMs = 0;
static uint32_t s_lastMetricsPrintMs = 0;

// Izbor obrasca statusne LED diode; važniji problem ima prednost (specifikacija §16.1)
static void updateStatusLed(uint32_t now) {
    using status_led::Pattern;
    sensor_manager::Health h = sensor_manager::health();

    Pattern pattern;
    bool noSensorWorks = h.total > 0 && h.ok == 0 && h.initializing == 0;
    if (!i2c_bus::ready(Wire) || noSensorWorks || ESP.getFreeHeap() < LOW_HEAP_BYTES) {
        pattern = Pattern::Critical;
    } else if (h.faulty > 0 || !i2c_bus::pullupsOk(Wire) || !i2c_bus::pullupsOk(Wire1)) {
        pattern = Pattern::SensorFault;
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
    i2c_bus::begin();
    i2c_bus::printScan();
    sensor_manager::begin();
    printHelp();
}

void loop() {
    esp_task_wdt_reset();

    uint32_t now = millis();

    sensor_manager::poll(now);
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
