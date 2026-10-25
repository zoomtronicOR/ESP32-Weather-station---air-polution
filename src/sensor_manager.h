#pragma once

#include <Arduino.h>

// Upravljanje svim senzorima: inicijalizacija, ponovni pokušaji i neblokirajuće čitanje

namespace sensor_manager {

struct Health {
    uint8_t total;         // senzori koji nisu isključeni
    uint8_t ok;
    uint8_t initializing;  // pronađeni, čeka se prvo validno merenje
    uint8_t faulty;        // nisu pronađeni ili su u grešci
};

void begin();
Health health();
void poll(uint32_t now);
void printStatus(uint32_t now);

}  // namespace sensor_manager
