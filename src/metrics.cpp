#include "metrics.h"

#include <math.h>

namespace metrics {

static const MetricDef DEFS[] = {
    // key                     apiKey                      naziv                       jedinica  dec  min     max       maxAge  izvor     trend
    {"temperature", "temperature_c",                        "Temperatura",              "°C",     1,  -40.0f,  85.0f,    20000, "bme680", 0.3f},
    {"humidity", "humidity_pct",                            "Relativna vlažnost",       "%",      1,    0.0f,  100.0f,   20000, "bme680", 2.0f},
    {"pressure", "pressure_hpa",                            "Atmosferski pritisak",     "hPa",    1,  300.0f,  1100.0f,  20000, "bme680", 1.0f},
    {"gas_resistance", "gas_resistance_ohm",                "Gasni otpor",              "Ω",      0,    1.0f,  1.0e8f,   30000, "bme680", 5000.0f},
    {"pm1_0", "pm1_0_ugm3",                                 "PM1.0",                    "µg/m³",  1,    0.0f,  1000.0f,  20000, "sps30", 2.0f},
    {"pm2_5", "pm2_5_ugm3",                                 "PM2.5",                    "µg/m³",  1,    0.0f,  1000.0f,  20000, "sps30", 2.0f},
    {"pm4", "pm4_ugm3",                                     "PM4",                      "µg/m³",  1,    0.0f,  1000.0f,  20000, "sps30", 2.0f},
    {"pm10", "pm10_ugm3",                                   "PM10",                     "µg/m³",  1,    0.0f,  1000.0f,  20000, "sps30", 3.0f},
    {"typical_particle_size", "typical_particle_size_um",   "Tipična veličina čestica", "µm",     2,    0.0f,  10.0f,    20000, "sps30", 0.05f},
    {"uv_index", "uv_index",                                "UV indeks",                "",       1,    0.0f,  20.0f,    20000, "ltr390", 0.5f},
    {"illuminance", "illuminance_lux",                      "Osvetljenost",             "lx",     0,    0.0f,  200000.0f,20000, "ltr390", 50.0f},
    {"pitch", "pitch_deg",                                  "Nagib (pitch)",            "°",      1,  -90.0f,  90.0f,    2000,  "mpu", 2.0f},
    {"roll", "roll_deg",                                    "Nagib (roll)",             "°",      1, -180.0f,  180.0f,   2000,  "mpu", 2.0f},
    {"motion", "motion",                                    "Pomeranje",                "",       0,    0.0f,  1.0f,     2000,  "mpu", 0.1f},
    {"air_quality_level", "air_quality_level",              "Kvalitet vazduha (EAQI)",  "",       0,    1.0f,  6.0f,     20000, "eaqi", 0.5f},
};

static_assert(sizeof(DEFS) / sizeof(DEFS[0]) == (size_t)MetricId::Count, "DEFS mora pratiti MetricId");

static Reading s_readings[(size_t)MetricId::Count];

void update(MetricId id, float value, uint32_t now) {
    const MetricDef &d = DEFS[(size_t)id];
    Reading &r = s_readings[(size_t)id];
    r.value = value;
    r.timestampMs = now;
    r.everUpdated = true;
    if (isnan(value) || value < d.minValid || value > d.maxValid) {
        r.valid = false;
        r.error = ErrorCode::OutOfRange;
    } else {
        r.valid = true;
        r.error = ErrorCode::None;
    }
}

void invalidate(MetricId id, ErrorCode code) {
    Reading &r = s_readings[(size_t)id];
    r.valid = false;
    r.error = code;
}

const MetricDef &def(MetricId id) { return DEFS[(size_t)id]; }

const Reading &get(MetricId id) { return s_readings[(size_t)id]; }

bool isFresh(MetricId id, uint32_t now) {
    const Reading &r = s_readings[(size_t)id];
    return r.valid && (now - r.timestampMs) <= DEFS[(size_t)id].maxAgeMs;
}

// Ispis naziva poravnatog na zadatu širinu; UTF-8 slova (č, ć, š...) zauzimaju 2 bajta, a jedno mesto
static void printPadded(const char *text, size_t width) {
    size_t chars = 0;
    for (const char *p = text; *p; p++) {
        if ((*p & 0xC0) != 0x80) chars++;
    }
    Serial.print(text);
    for (; chars < width; chars++) Serial.print(' ');
}

void printAll(uint32_t now) {
    Serial.println();
    Serial.println("==================== Merenja ====================");
    for (size_t i = 0; i < (size_t)MetricId::Count; i++) {
        const MetricDef &d = DEFS[i];
        const Reading &r = s_readings[i];
        Serial.print("  ");
        printPadded(d.name, 27);
        if (!r.everUpdated && r.error == ErrorCode::None) {
            Serial.println("nepoznato");
        } else if (isFresh((MetricId)i, now)) {
            Serial.printf("%.*f %s\n", d.decimals, r.value, d.unit);
        } else {
            const char *why = r.valid ? "zastarelo" : errorCodeText(r.error);
            Serial.printf("nedostupno (%s)\n", why);
        }
    }
    Serial.println("=================================================");
    Serial.println();
}

}  // namespace metrics

const char *sensorStatusText(SensorStatus status) {
    switch (status) {
        case SensorStatus::NotFound:     return "nije pronađen";
        case SensorStatus::Initializing: return "inicijalizacija";
        case SensorStatus::Ok:           return "ispravan";
        case SensorStatus::Error:        return "greška";
        case SensorStatus::Disabled:     return "isključen";
    }
    return "?";
}

const char *errorCodeText(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:       return "bez greške";
        case ErrorCode::NotFound:   return "senzor nije pronađen";
        case ErrorCode::Comm:       return "greška komunikacije";
        case ErrorCode::OutOfRange: return "van opsega";
        case ErrorCode::Warmup:     return "zagrevanje";
        case ErrorCode::Hardware:   return "greška senzora";
    }
    return "?";
}
