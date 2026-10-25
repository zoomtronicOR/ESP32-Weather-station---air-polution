#pragma once

#include <Adafruit_LTR390.h>

#include "sensor_driver.h"

// LTR-390: ambijentalno svetlo (lux) i UV indeks, naizmenično ALS i UVS režim

class Ltr390Sensor : public SensorDriver {
public:
    const char *id() const override { return "ltr390"; }
    const char *name() const override { return "LTR-390"; }
    uint8_t bus() const override { return 0; }
    uint8_t address() const override { return 0x53; }

    bool begin(uint32_t now) override;
    void poll(uint32_t now) override;
    void invalidateMetrics(ErrorCode code) override;

private:
    enum class Phase : uint8_t { Idle, WaitAls, WaitUvs };

    void startAls(uint32_t now);
    void startUvs(uint32_t now);

    Adafruit_LTR390 ltr_;
    Phase phase_ = Phase::Idle;
    uint32_t lastCycleMs_ = 0;
    uint32_t phaseStartMs_ = 0;
    uint8_t samplesToSkip_ = 0;
};
