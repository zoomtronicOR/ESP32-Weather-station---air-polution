#include "air_quality.h"

#include "config.h"
#include "log.h"

namespace air_quality {

static const char *TAG = "eaqi";

// Gornje granice nivoa 1..5 (µg/m³); iznad poslednje je nivo 6.
// Početni predlog prema revidiranom EAQI -- proveriti sa zvaničnom verzijom (specifikacija §10A.1).
static const float PM25_LIMITS[5] = {5.0f, 15.0f, 50.0f, 90.0f, 140.0f};
static const float PM10_LIMITS[5] = {15.0f, 45.0f, 120.0f, 195.0f, 270.0f};

static constexpr size_t WINDOW_SAMPLES = AQ_WINDOW_MS / SPS30_INTERVAL_MS;

struct Sample {
    float pm25;
    float pm10;
    uint32_t ms;
};

static Sample s_samples[WINDOW_SAMPLES];
static size_t s_head = 0;
static size_t s_count = 0;
static uint32_t s_lastSampleMs = 0;
static uint32_t s_lastPm25Ts = 0;

static uint8_t s_level = 0;
static uint8_t s_candidate = 0;
static uint32_t s_candidateSinceMs = 0;
static MetricId s_dominant = MetricId::Pm2_5;
static float s_dominantAvg = NAN;

static uint8_t levelFor(float value, const float *limits) {
    uint8_t lvl = 1;
    for (uint8_t i = 0; i < 5; i++) {
        if (value > limits[i]) lvl = i + 2;
    }
    return lvl;
}

void update(uint32_t now) {
    // Novi uzorak samo kada SPS30 da novo validno merenje
    const Reading &pm25 = metrics::get(MetricId::Pm2_5);
    if (metrics::isFresh(MetricId::Pm2_5, now) && metrics::isFresh(MetricId::Pm10, now) &&
        pm25.timestampMs != s_lastPm25Ts) {
        s_lastPm25Ts = pm25.timestampMs;
        s_samples[s_head] = {pm25.value, metrics::get(MetricId::Pm10).value, now};
        s_head = (s_head + 1) % WINDOW_SAMPLES;
        if (s_count < WINDOW_SAMPLES) s_count++;
        s_lastSampleMs = now;
    }

    // Bez svežih uzoraka nivo je nepoznat -- nikada se ne prikazuje "dobar" vazduh bez merenja
    if (s_count == 0 || now - s_lastSampleMs > AQ_STALE_MS) {
        if (s_level != 0) {
            LOG_W(TAG, "Nema svežih PM merenja -- nivo kvaliteta vazduha je nepoznat");
        }
        s_level = 0;
        s_candidate = 0;
        s_count = 0;
        metrics::invalidate(MetricId::AirQualityLevel, ErrorCode::Warmup);
        return;
    }

    float sum25 = 0, sum10 = 0;
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (now - s_samples[i].ms <= AQ_WINDOW_MS) {
            sum25 += s_samples[i].pm25;
            sum10 += s_samples[i].pm10;
            n++;
        }
    }
    if (n == 0) return;
    float avg25 = sum25 / n;
    float avg10 = sum10 / n;

    uint8_t lvl25 = levelFor(avg25, PM25_LIMITS);
    uint8_t lvl10 = levelFor(avg10, PM10_LIMITS);
    uint8_t newLevel = lvl25 >= lvl10 ? lvl25 : lvl10;
    s_dominant = lvl25 >= lvl10 ? MetricId::Pm2_5 : MetricId::Pm10;
    s_dominantAvg = lvl25 >= lvl10 ? avg25 : avg10;

    // Histereza: novi nivo mora da traje AQ_HYSTERESIS_MS (osim prvog određivanja)
    if (s_level == 0) {
        s_level = newLevel;
        s_candidate = newLevel;
        LOG_I(TAG, "Kvalitet vazduha: %s (%u/6)", levelName(s_level), s_level);
    } else if (newLevel != s_level) {
        if (newLevel != s_candidate) {
            s_candidate = newLevel;
            s_candidateSinceMs = now;
        } else if (now - s_candidateSinceMs >= AQ_HYSTERESIS_MS) {
            s_level = newLevel;
            LOG_I(TAG, "Kvalitet vazduha: %s (%u/6)", levelName(s_level), s_level);
        }
    } else {
        s_candidate = s_level;
    }

    metrics::update(MetricId::AirQualityLevel, s_level, now);
}

uint8_t level() { return s_level; }

const char *levelName(uint8_t lvl) {
    switch (lvl) {
        case 1: return "Dobar";
        case 2: return "Prihvatljiv";
        case 3: return "Umeren";
        case 4: return "Loš";
        case 5: return "Veoma loš";
        case 6: return "Izuzetno loš";
        default: return "nepoznat";
    }
}

MetricId dominant() { return s_dominant; }

float dominantAverage() { return s_dominantAvg; }

}  // namespace air_quality
