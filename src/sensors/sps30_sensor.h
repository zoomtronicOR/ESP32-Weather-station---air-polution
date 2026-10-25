#pragma once

#include <SensirionI2cSps30.h>

#include "sensor_driver.h"

// SPS30: PM1.0, PM2.5, PM4, PM10 na zasebnoj I2C magistrali (I2C1 / Wire1)

class Sps30Sensor : public SensorDriver {
public:
    const char *id() const override { return "sps30"; }
    const char *name() const override { return name_; }
    uint8_t bus() const override { return 1; }
    uint8_t address() const override { return SPS30_I2C_ADDR_69; }

    bool begin(uint32_t now) override;
    void poll(uint32_t now) override;
    void invalidateMetrics(ErrorCode code) override;

private:
    void checkDeviceStatus();

    SensirionI2cSps30 sps_;
    char name_[24] = "SPS30";
    uint32_t measureStartMs_ = 0;
    uint32_t lastPollMs_ = 0;
    uint32_t lastStatusCheckMs_ = 0;
};
