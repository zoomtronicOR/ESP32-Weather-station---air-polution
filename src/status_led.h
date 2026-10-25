#pragma once

#include <Arduino.h>

// Statusna (plava) LED dioda na ploči -- kod treptanja (specifikacija §16.1)
// Ciklus traje 2 s; broj kratkih bljeskova u ciklusu označava problem.

namespace status_led {

enum class Pattern : uint8_t {
    Solid,        // stalno upaljena: pokretanje u toku
    Heartbeat,    // 1 kratak bljesak / 2 s: sve ispravno
    Warmup,       // ravnomerno 1 Hz: zagrevanje / inicijalizacija senzora
    SensorFault,  // 2 bljeska / 2 s: problem sa senzorom ili ožičenjem
    NoWifi,       // 3 bljeska / 2 s: nema Wi-Fi veze
    NoMqtt,       // 4 bljeska / 2 s: nema MQTT veze
    Portal,       // dugačak bljesak 1 s / 2 s: aktivan konfiguracioni portal
    Critical,     // brzo treptanje 5 Hz: kritična greška
};

void begin();
void set(Pattern pattern);
void update(uint32_t now);

Pattern current();
const char *describe(Pattern pattern);

}  // namespace status_led
