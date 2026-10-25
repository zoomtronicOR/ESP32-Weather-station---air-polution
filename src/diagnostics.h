#pragma once

#include <Arduino.h>
#include <esp_system.h>

// Dijagnostika: podaci o čipu, razlog restarta, brojač pokretanja, memorija

namespace diagnostics {

void begin();

void printChipInfo();
void printStatus();

uint32_t bootCount();
esp_reset_reason_t resetReason();
const char *resetReasonText(esp_reset_reason_t reason);

}  // namespace diagnostics
