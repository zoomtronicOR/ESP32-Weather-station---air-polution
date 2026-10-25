#pragma once

#include <Arduino.h>

// MQTT: objavljivanje merenja, LWT/availability i Home Assistant MQTT Discovery (specifikacija §8)
// Klijent je ESP-IDF esp-mqtt: radi u sopstvenom zadatku i ne blokira glavnu petlju.

namespace mqtt_manager {

void begin();
void update(uint32_t now, bool networkUp);
// Nova MQTT podešavanja -- klijent se zaustavlja i ponovo pokreće
void restart();

bool enabled();
bool connected();
const char *lastError();
uint32_t publishCount();
uint32_t lastPublishAgeS();  // 0 ako još nije bilo objave
uint32_t connectCount();

}  // namespace mqtt_manager
