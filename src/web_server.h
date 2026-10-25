#pragma once

#include <Arduino.h>

// Lokalni web server: web interfejs iz LittleFS-a i REST API /api/v1 (specifikacija §6, §13)

namespace web_server {

void begin();
// Izvršava akcije zatražene preko API-ja (izmena podešavanja, restart...) u glavnoj petlji
void update(uint32_t now);

}  // namespace web_server
