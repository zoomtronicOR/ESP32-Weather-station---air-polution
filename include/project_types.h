#pragma once

#include <Arduino.h>

// Zajednički tipovi podataka firmware-a

enum class SensorStatus : uint8_t {
    NotFound,      // senzor nije pronađen
    Initializing,  // pronađen, čeka se prvo validno merenje (npr. zagrevanje)
    Ok,            // radi ispravno
    Error,         // previše uzastopnih grešaka, čeka se ponovna inicijalizacija
    Disabled,      // isključen u podešavanjima
};

enum class ErrorCode : uint8_t {
    None = 0,
    NotFound,      // uređaj ne odgovara
    Comm,          // greška u komunikaciji
    OutOfRange,    // vrednost van dozvoljenog opsega
    Warmup,        // senzor se zagreva, merenje još nije pouzdano
    Hardware,      // senzor prijavljuje sopstvenu grešku (npr. ventilator)
};

// Jedno očitavanje jedne veličine (specifikacija §5.3)
struct Reading {
    float value = NAN;
    uint32_t timestampMs = 0;  // uptime u trenutku merenja
    bool valid = false;
    bool everUpdated = false;  // razlikuje "nepoznato" od "nedostupno"
    ErrorCode error = ErrorCode::None;
};

const char *sensorStatusText(SensorStatus status);
const char *errorCodeText(ErrorCode code);
