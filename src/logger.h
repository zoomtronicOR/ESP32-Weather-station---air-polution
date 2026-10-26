#pragma once

#include <Arduino.h>
#include <functional>

#include "log.h"

// Logovi: ispis na serijski port i kružni bafer u RAM-u za prikaz u web interfejsu (specifikacija §16)

namespace logger {

struct Entry {
    uint32_t ms;     // uptime
    uint8_t level;   // 0 = ERROR ... 3 = DEBUG
    char tag[12];
    char msg[112];
};

// Nivo u radu (0–3); poruke detaljnije od ovog nivoa se ne ispisuju ni ne čuvaju
void setLevel(uint8_t level);
uint8_t level();
const char *levelName(uint8_t level);

// Prolaz kroz sačuvane zapise od najstarijeg; bezbedno iz web zadatka
void forEach(const std::function<void(const Entry &)> &fn);
uint32_t droppedCount();

}  // namespace logger
