#pragma once

#include <Arduino.h>
#include <time.h>

// Vreme i datum: NTP sinhronizacija, vremenska zona, statusi (specifikacija §12)
// Interno se vreme čuva kao UTC (Unix epoch); lokalno se prikazuje u podešenoj zoni.

namespace time_manager {

enum class TimeStatus : uint8_t {
    NotSet,   // vreme nije postavljeno od restarta
    Syncing,  // sinhronizacija u toku
    Synced,   // sinhronizovano preko NTP-a
    Stale,    // poslednja sinhronizacija starija od 24 h
    Manual,   // ručno podešeno iz web interfejsa
};

void begin();
void update(uint32_t now, bool networkUp);
// Ponovo primenjuje NTP podešavanja i vremensku zonu iz konfiguracije
void applyConfig();

TimeStatus status();
const char *statusKey(TimeStatus s);   // not_set, syncing, synced, stale, manual
bool isValid();                       // synced, stale ili manual

time_t nowUtc();
// Epoch merenja izračunat iz uptime-a; retroaktivno ispravno i za merenja pre sinhronizacije
time_t epochFromUptime(uint32_t uptimeMs);

bool setManual(time_t epoch);

time_t lastSyncUtc();
int32_t lastOffsetMs();
uint32_t syncCount();

// Formatiranje
void formatIsoUtc(time_t t, char *buf, size_t len);    // 2026-10-25T13:35:07Z
void formatIsoLocal(time_t t, char *buf, size_t len);  // 2026-10-25T14:35:07+01:00
const char *tzAbbrev();                                // CET / CEST
// "14:35" i "25.10." za OLED; vraća false ako vreme nije pouzdano
bool formatClock(char *time, size_t tlen, char *date, size_t dlen);

}  // namespace time_manager
