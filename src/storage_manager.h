#pragma once

#include <Arduino.h>
#include <functional>

#include "metrics.h"

// Lokalna istorija merenja u LittleFS (specifikacija §9)
// - 5-minutni agregati (min/max/prosek/broj) za poslednja 24 h
// - satni agregati za poslednjih 30 dana
// Oba skupa su kružni fajlovi fiksne veličine; najstariji zapisi se prepisuju.
// U flash se upisuje jednom na 5 min, nikada svaki uzorak.

namespace storage_manager {

struct __attribute__((packed)) MetricAgg {
    float avg;
    float min;
    float max;
    uint16_t count;  // 0 = nema validnih merenja u intervalu
};

struct __attribute__((packed)) HistRecord {
    uint32_t ts;  // početak intervala, Unix epoch (UTC)
    MetricAgg m[(size_t)MetricId::Count];
};

enum class Resolution : uint8_t { Min5, Hour };

void begin();
void update(uint32_t now);

// Briše celu istoriju (poziva se iz glavne petlje)
void clear();

size_t count(Resolution r);
size_t capacity(Resolution r);
uint32_t intervalSeconds(Resolution r);
size_t pendingCount();  // intervali koji čekaju pouzdano vreme
size_t fsTotalBytes();  // particija "history"
size_t fsUsedBytes();

// Prolaz kroz zapise od najstarijeg ka najnovijem, sa ts >= from.
// Bezbedno za poziv iz web zadatka (zaključava pristup fajlu).
void forEach(Resolution r, uint32_t from, const std::function<void(const HistRecord &)> &fn);

// Čita zapis po rednom broju od najstarijeg (0 = najstariji); false ako ne postoji
bool readAt(Resolution r, size_t index, HistRecord &out);
// Čita do n uzastopnih zapisa počev od rednog broja start; vraća broj pročitanih
size_t readRange(Resolution r, size_t start, HistRecord *out, size_t n);

}  // namespace storage_manager
