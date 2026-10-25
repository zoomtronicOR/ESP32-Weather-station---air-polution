#pragma once

#include "project_types.h"

// Registar metrika: definicije veličina i poslednja očitavanja (specifikacija §4A.4, §5.2, §5.3)

enum class MetricId : uint8_t {
    Temperature,
    Humidity,
    Pressure,
    GasResistance,
    Pm1_0,
    Pm2_5,
    Pm4,
    Pm10,
    ParticleSize,
    UvIndex,
    Illuminance,
    Pitch,
    Roll,
    Motion,
    Count
};

struct MetricDef {
    const char *key;     // ključ za MQTT/JSON
    const char *name;    // naziv na srpskom
    const char *unit;
    uint8_t decimals;
    float minValid;
    float maxValid;
    uint32_t maxAgeMs;   // starije očitavanje se smatra zastarelim
    const char *source;  // id senzora
};

namespace metrics {

void update(MetricId id, float value, uint32_t now);
void invalidate(MetricId id, ErrorCode code);

const MetricDef &def(MetricId id);
const Reading &get(MetricId id);

// Validno i nije zastarelo
bool isFresh(MetricId id, uint32_t now);

void printAll(uint32_t now);

}  // namespace metrics
