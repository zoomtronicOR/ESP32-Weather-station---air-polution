#include "sensors/ltr390_sensor.h"

#include "i2c_bus.h"
#include "metrics.h"

static constexpr uint8_t LTR390_PART_ID_REG = 0x06;

// ALS: pojačanje 3, 18 bita (100 ms); UVS: pojačanje 18, 20 bita (400 ms)
static constexpr float ALS_GAIN = 3.0f;
static constexpr float ALS_INT_FACTOR = 1.0f;     // 100 ms / 100 ms
static constexpr uint32_t ALS_TIME_MS = 100;
static constexpr uint32_t UVS_TIME_MS = 400;
// Osetljivost iz datasheet-a: 2300 impulsa po UV indeksu pri pojačanju 18 i 20 bita
static constexpr float UVS_SENSITIVITY = 2300.0f;
static constexpr uint32_t PHASE_TIMEOUT_MS = 3000;

bool Ltr390Sensor::begin(uint32_t now) {
    // Identitet: gornja 4 bita PART_ID registra = 0xB
    uint8_t partId = 0;
    if (!i2c_bus::readRegister(Wire, 0x53, LTR390_PART_ID_REG, partId) || (partId >> 4) != 0x0B) {
        return false;
    }
    if (!ltr_.begin(&Wire)) {
        return false;
    }
    phase_ = Phase::Idle;
    lastCycleMs_ = now - LTR390_INTERVAL_MS;  // prvo merenje odmah
    consecutiveErrors_ = 0;
    status_ = SensorStatus::Initializing;
    return true;
}

void Ltr390Sensor::startAls(uint32_t now) {
    ltr_.setMode(LTR390_MODE_ALS);
    ltr_.setGain(LTR390_GAIN_3);
    ltr_.setResolution(LTR390_RESOLUTION_18BIT);
    ltr_.newDataAvailable();  // briše eventualni stari indikator
    samplesToSkip_ = 1;       // prvo merenje posle promene režima se odbacuje
    phase_ = Phase::WaitAls;
    phaseStartMs_ = now;
}

void Ltr390Sensor::startUvs(uint32_t now) {
    ltr_.setMode(LTR390_MODE_UVS);
    ltr_.setGain(LTR390_GAIN_18);
    ltr_.setResolution(LTR390_RESOLUTION_20BIT);
    ltr_.newDataAvailable();
    samplesToSkip_ = 1;
    phase_ = Phase::WaitUvs;
    phaseStartMs_ = now;
}

void Ltr390Sensor::poll(uint32_t now) {
    switch (phase_) {
        case Phase::Idle:
            if (now - lastCycleMs_ >= LTR390_INTERVAL_MS) {
                lastCycleMs_ = now;
                startAls(now);
            }
            break;

        case Phase::WaitAls:
            if (now - phaseStartMs_ > PHASE_TIMEOUT_MS) {
                phase_ = Phase::Idle;
                markError(ErrorCode::Comm);
            } else if (now - phaseStartMs_ >= ALS_TIME_MS && ltr_.newDataAvailable()) {
                uint32_t als = ltr_.readALS();
                if (samplesToSkip_ > 0) {
                    samplesToSkip_--;
                    break;
                }
                float lux = 0.6f * als / (ALS_GAIN * ALS_INT_FACTOR) * LTR390_WFAC;
                metrics::update(MetricId::Illuminance, lux, now);
                startUvs(now);
            }
            break;

        case Phase::WaitUvs:
            if (now - phaseStartMs_ > PHASE_TIMEOUT_MS) {
                phase_ = Phase::Idle;
                markError(ErrorCode::Comm);
            } else if (now - phaseStartMs_ >= UVS_TIME_MS && ltr_.newDataAvailable()) {
                uint32_t uvs = ltr_.readUVS();
                if (samplesToSkip_ > 0) {
                    samplesToSkip_--;
                    break;
                }
                float uvi = uvs / UVS_SENSITIVITY * LTR390_WFAC * LTR390_UVI_CALIBRATION;
                metrics::update(MetricId::UvIndex, uvi, now);
                phase_ = Phase::Idle;
                markOk(now);
            }
            break;
    }
}

void Ltr390Sensor::invalidateMetrics(ErrorCode code) {
    metrics::invalidate(MetricId::Illuminance, code);
    metrics::invalidate(MetricId::UvIndex, code);
}
