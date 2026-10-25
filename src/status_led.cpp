#include "status_led.h"

#include "log.h"
#include "pins.h"

namespace status_led {

static const char *TAG = "led";

static constexpr uint32_t CYCLE_MS = 2000;
static constexpr uint32_t FLASH_MS = 100;       // trajanje kratkog bljeska
static constexpr uint32_t FLASH_PERIOD_MS = 300; // bljesak + razmak u nizu

static Pattern s_pattern = Pattern::Solid;
static uint32_t s_patternStartMs = 0;
static bool s_ledOn = false;

static bool codeFlash(uint32_t t, uint8_t count) {
    uint32_t p = t % CYCLE_MS;
    return p < count * FLASH_PERIOD_MS && (p % FLASH_PERIOD_MS) < FLASH_MS;
}

static bool isOn(Pattern pattern, uint32_t t) {
    switch (pattern) {
        case Pattern::Solid:       return true;
        case Pattern::Heartbeat:   return codeFlash(t, 1);
        case Pattern::Warmup:      return (t % 1000) < 500;
        case Pattern::SensorFault: return codeFlash(t, 2);
        case Pattern::NoWifi:      return codeFlash(t, 3);
        case Pattern::NoMqtt:      return codeFlash(t, 4);
        case Pattern::Portal:      return (t % CYCLE_MS) < 1000;
        case Pattern::Critical:    return (t % 200) < 100;
    }
    return false;
}

void begin() {
    pinMode(PIN_STATUS_LED, OUTPUT);
    s_pattern = Pattern::Solid;
    s_patternStartMs = millis();
    s_ledOn = true;
    digitalWrite(PIN_STATUS_LED, HIGH);
}

void set(Pattern pattern) {
    if (pattern == s_pattern) {
        return;
    }
    LOG_I(TAG, "Statusna LED: %s", describe(pattern));
    s_pattern = pattern;
    s_patternStartMs = millis();  // novi obrazac počinje od početka ciklusa
}

void update(uint32_t now) {
    bool on = isOn(s_pattern, now - s_patternStartMs);
    if (on != s_ledOn) {
        s_ledOn = on;
        digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
    }
}

Pattern current() { return s_pattern; }

const char *describe(Pattern pattern) {
    switch (pattern) {
        case Pattern::Solid:       return "stalno upaljena (pokretanje)";
        case Pattern::Heartbeat:   return "1 bljesak / 2 s (sve ispravno)";
        case Pattern::Warmup:      return "1 Hz (zagrevanje senzora)";
        case Pattern::SensorFault: return "2 bljeska / 2 s (problem sa senzorom)";
        case Pattern::NoWifi:      return "3 bljeska / 2 s (nema Wi-Fi veze)";
        case Pattern::NoMqtt:      return "4 bljeska / 2 s (nema MQTT veze)";
        case Pattern::Portal:      return "dugačak bljesak / 2 s (konfiguracioni portal)";
        case Pattern::Critical:    return "brzo treptanje 5 Hz (kritična greška)";
    }
    return "?";
}

}  // namespace status_led
