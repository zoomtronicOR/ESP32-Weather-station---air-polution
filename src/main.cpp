// ESP32 R201 -- Pametna meteo stanica
// Faza 1: osnovni test ploče (identifikacija čipa, serijska veza, restart, watchdog)

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "diagnostics.h"
#include "log.h"
#include "pins.h"

static const char *TAG = "main";

static uint32_t s_lastStatusMs = 0;
static uint32_t s_lastBlinkMs = 0;
static bool s_ledOn = false;

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

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    LOG_I(TAG, "ESP32 meteo stanica, firmware %s -- Faza 1", FW_VERSION);
    diagnostics::begin();
    diagnostics::printChipInfo();
    setupWatchdog();
    printHelp();
}

void loop() {
    esp_task_wdt_reset();

    uint32_t now = millis();

    // Heartbeat: ugrađena LED dioda trepće jednom u sekundi
    if (now - s_lastBlinkMs >= 500) {
        s_lastBlinkMs = now;
        s_ledOn = !s_ledOn;
        digitalWrite(PIN_STATUS_LED, s_ledOn ? HIGH : LOW);
    }

    if (now - s_lastStatusMs >= STATUS_INTERVAL_MS) {
        s_lastStatusMs = now;
        diagnostics::printStatus();
    }

    handleSerial();
    delay(10);
}
