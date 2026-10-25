#pragma once

#include <Adafruit_BME680.h>

#include "sensor_driver.h"

// BME680: temperatura, vlažnost, pritisak i gasni otpor (asinhrono merenje)

class Bme680Sensor : public SensorDriver {
public:
    const char *id() const override { return "bme680"; }
    const char *name() const override { return "BME680"; }
    uint8_t bus() const override { return 0; }
    uint8_t address() const override { return address_; }

    bool begin(uint32_t now) override;
    void poll(uint32_t now) override;
    void invalidateMetrics(ErrorCode code) override;

private:
    void publish(uint32_t now);

    Adafruit_BME680 bme_{&Wire};
    uint8_t address_ = 0x77;
    bool measuring_ = false;
    bool gasCycle_ = false;
    uint32_t lastStartMs_ = 0;
};
