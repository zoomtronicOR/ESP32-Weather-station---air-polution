#pragma once

#include <Arduino.h>

#include "display_manager.h"

// Wi-Fi: povezivanje, ponovno povezivanje sa backoff-om i konfiguracioni portal (specifikacija §7)

namespace wifi_manager {

void begin(uint32_t now);
void update(uint32_t now);

bool connected();
bool portalActive();
bool hasCredentials();

// Ručno pokretanje portala (taster BOOT ili web interfejs)
void startPortal(uint32_t now);
// Nova Wi-Fi podešavanja su sačuvana -- ponovo se povezati
void reconnect(uint32_t now);

ConnectionInfo connectionInfo();
uint32_t reconnectCount();
uint32_t connectedSinceMs();  // 0 ako nije povezan

}  // namespace wifi_manager
