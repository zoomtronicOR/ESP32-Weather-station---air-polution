#include "sensors/sps30_sensor.h"

#include <Wire.h>

#include "i2c_bus.h"
#include "metrics.h"

static constexpr uint32_t STATUS_CHECK_MS = 60000;

// Biti statusnog registra SPS30
static constexpr uint32_t STATUS_FAN_SPEED_WARNING = 1UL << 21;
static constexpr uint32_t STATUS_LASER_ERROR = 1UL << 5;
static constexpr uint32_t STATUS_FAN_ERROR = 1UL << 4;

bool Sps30Sensor::begin(uint32_t now) {
    if (!i2c_bus::probe(Wire1, SPS30_I2C_ADDR_69)) {
        return false;
    }
    sps_.begin(Wire1, SPS30_I2C_ADDR_69);
    sps_.stopMeasurement();  // u slučaju da merenje radi od ranije
    delay(25);

    // Identitet: SPS30 mora da vrati serijski broj
    int8_t serial[32] = {0};
    if (sps_.readSerialNumber(serial, sizeof(serial)) != 0 || serial[0] == 0) {
        return false;
    }
    uint8_t major = 0, minor = 0;
    if (sps_.readFirmwareVersion(major, minor) == 0) {
        snprintf(name_, sizeof(name_), "SPS30 fw %u.%u", major, minor);
    }
    if (sps_.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT) != 0) {
        return false;
    }
    LOG_D(id(), "serijski broj %s", (const char *)serial);

    measureStartMs_ = now;
    lastPollMs_ = now;
    lastStatusCheckMs_ = now;
    consecutiveErrors_ = 0;
    status_ = SensorStatus::Initializing;
    invalidateMetrics(ErrorCode::Warmup);
    return true;
}

void Sps30Sensor::checkDeviceStatus() {
    uint32_t deviceStatus = 0;
    if (sps_.readDeviceStatusRegister(deviceStatus) != 0) {
        markError(ErrorCode::Comm);
        return;
    }
    if (deviceStatus & STATUS_FAN_SPEED_WARNING) {
        LOG_W(id(), "Brzina ventilatora van opsega");
    }
    if (deviceStatus & (STATUS_LASER_ERROR | STATUS_FAN_ERROR)) {
        LOG_E(id(), "Greška %s%s", (deviceStatus & STATUS_LASER_ERROR) ? "lasera " : "",
              (deviceStatus & STATUS_FAN_ERROR) ? "ventilatora" : "");
        markError(ErrorCode::Hardware);
    }
}

void Sps30Sensor::poll(uint32_t now) {
    if (now - lastStatusCheckMs_ >= STATUS_CHECK_MS) {
        lastStatusCheckMs_ = now;
        checkDeviceStatus();
    }

    if (now - lastPollMs_ < SPS30_INTERVAL_MS) {
        return;
    }
    lastPollMs_ = now;

    uint16_t ready = 0;
    if (sps_.readDataReadyFlag(ready) != 0) {
        markError(ErrorCode::Comm);
        return;
    }
    if (!ready) {
        return;  // SPS30 daje novo merenje svake sekunde
    }

    float mc1p0, mc2p5, mc4p0, mc10p0, nc0p5, nc1p0, nc2p5, nc4p0, nc10p0, typicalSize;
    if (sps_.readMeasurementValuesFloat(mc1p0, mc2p5, mc4p0, mc10p0, nc0p5, nc1p0, nc2p5, nc4p0, nc10p0,
                                        typicalSize) != 0) {
        markError(ErrorCode::Comm);
        return;
    }

    if (now - measureStartMs_ < SPS30_WARMUP_MS) {
        // Zagrevanje: komunikacija radi, ali merenje se još ne prikazuje kao ispravno
        consecutiveErrors_ = 0;
        status_ = SensorStatus::Initializing;
        invalidateMetrics(ErrorCode::Warmup);
        return;
    }

    metrics::update(MetricId::Pm1_0, mc1p0, now);
    metrics::update(MetricId::Pm2_5, mc2p5, now);
    metrics::update(MetricId::Pm4, mc4p0, now);
    metrics::update(MetricId::Pm10, mc10p0, now);
    metrics::update(MetricId::ParticleSize, typicalSize, now);
    markOk(now);
}

void Sps30Sensor::invalidateMetrics(ErrorCode code) {
    metrics::invalidate(MetricId::Pm1_0, code);
    metrics::invalidate(MetricId::Pm2_5, code);
    metrics::invalidate(MetricId::Pm4, code);
    metrics::invalidate(MetricId::Pm10, code);
    metrics::invalidate(MetricId::ParticleSize, code);
}
