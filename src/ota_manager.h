#pragma once

#include <Arduino.h>

// OTA ažuriranje firmware-a i web fajlova (specifikacija §15)
// - ArduinoOTA (espota, port 3232) za razvoj: pio run -e ota -t upload / uploadfs
// - Upload .bin fajla iz web interfejsa (Sistem)
// Obe putanje traže administratorsku lozinku. Novi firmware se potvrđuje tek posle
// stabilnog rada; ako se pre toga sruši, bootloader vraća prethodnu verziju (rollback).

namespace ota_manager {

void begin();
void update(uint32_t now, bool networkUp);
// Nova administratorska lozinka ili naziv uređaja
void applyConfig();

bool inProgress();
uint8_t progress();       // 0–100
const char *target();     // "firmware" ili "web fajlovi"

// Upload iz web interfejsa (poziva se iz web zadatka)
bool webBegin(bool filesystem, size_t size, String &error);
bool webWrite(uint8_t *data, size_t len, size_t total);
bool webEnd(String &error);
void webAbort();

// Namerni restart (web, Home Assistant, serijska komanda, posle ažuriranja web fajlova).
// Nepotvrđeni novi firmware se prvo potvrđuje: namerni restart nije pad, pa ne sme da izazove
// vraćanje na prethodnu verziju.
void restartNow();

const char *runningPartition();
bool pendingVerify();     // novi firmware još nije potvrđen

}  // namespace ota_manager
