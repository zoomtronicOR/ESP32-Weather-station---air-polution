#include "sensors/bme680_sensor.h"

#include "i2c_bus.h"
#include "metrics.h"

static constexpr uint8_t BME680_CHIP_ID_REG = 0xD0;
static constexpr uint8_t BME680_CHIP_ID = 0x61;
static constexpr uint32_t MEASURE_TIMEOUT_MS = 2000;

bool Bme680Sensor::begin(uint32_t now) {
    // Identitet se potvrđuje registrom chip_id, ne samo adresom
    const uint8_t candidates[] = {0x77, 0x76};
    bool found = false;
    for (uint8_t addr : candidates) {
        uint8_t chipId = 0;
        if (i2c_bus::readRegister(Wire, addr, BME680_CHIP_ID_REG, chipId) && chipId == BME680_CHIP_ID) {
            address_ = addr;
            found = true;
            break;
        }
    }
    if (!found || !bme_.begin(address_, true)) {
        return false;
    }

    bme_.setTemperatureOversampling(BME680_OS_8X);
    bme_.setHumidityOversampling(BME680_OS_2X);
    bme_.setPressureOversampling(BME680_OS_4X);
    bme_.setIIRFilterSize(BME680_FILTER_SIZE_3);

    measuring_ = false;
    gasCycle_ = false;
    lastStartMs_ = now - BME680_INTERVAL_MS;  // prvo merenje odmah
    consecutiveErrors_ = 0;
    status_ = SensorStatus::Initializing;
    return true;
}

void Bme680Sensor::poll(uint32_t now) {
    if (!measuring_) {
        if (now - lastStartMs_ < BME680_INTERVAL_MS) {
            return;
        }
        // Gasni otpor na svako drugo merenje: manje samozagrevanja od grejača
        gasCycle_ = !gasCycle_;
        if (gasCycle_) {
            bme_.setGasHeater(BME680_HEATER_TEMP_C, BME680_HEATER_MS);
        } else {
            bme_.setGasHeater(0, 0);
        }
        lastStartMs_ = now;
        if (bme_.beginReading() == 0) {
            markError(ErrorCode::Comm);
            return;
        }
        measuring_ = true;
        return;
    }

    int remaining = bme_.remainingReadingMillis();
    if (remaining == Adafruit_BME680::reading_complete) {
        measuring_ = false;
        if (bme_.endReading()) {  // merenje je završeno, nema čekanja
            publish(now);
        } else {
            markError(ErrorCode::Comm);
        }
    } else if (remaining == Adafruit_BME680::reading_not_started || now - lastStartMs_ > MEASURE_TIMEOUT_MS) {
        measuring_ = false;
        markError(ErrorCode::Comm);
    }
}

void Bme680Sensor::publish(uint32_t now) {
    metrics::update(MetricId::Temperature, bme_.temperature, now);
    metrics::update(MetricId::Humidity, bme_.humidity, now);
    metrics::update(MetricId::Pressure, bme_.pressure / 100.0f, now);
    if (gasCycle_) {
        if (bme_.gas_resistance > 0) {
            metrics::update(MetricId::GasResistance, (float)bme_.gas_resistance, now);
        } else {
            // Grejač nije stabilan -- vrednost se ne prikazuje kao ispravna
            metrics::invalidate(MetricId::GasResistance, ErrorCode::Warmup);
        }
    }
    markOk(now);
}

void Bme680Sensor::invalidateMetrics(ErrorCode code) {
    metrics::invalidate(MetricId::Temperature, code);
    metrics::invalidate(MetricId::Humidity, code);
    metrics::invalidate(MetricId::Pressure, code);
    metrics::invalidate(MetricId::GasResistance, code);
}
