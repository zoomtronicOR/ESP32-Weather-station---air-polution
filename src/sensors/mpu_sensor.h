#pragma once

#include "sensor_driver.h"

// MPU6500 / MPU9250 / MPU9255: akcelerometar i žiroskop -> nagib i detekcija pomeranja
// Ugrađen je MPU6500 (bez magnetometra); varijanta se prepoznaje preko WHO_AM_I

class MpuSensor : public SensorDriver {
public:
    const char *id() const override { return "mpu"; }
    const char *name() const override { return name_; }
    uint8_t bus() const override { return 0; }
    uint8_t address() const override { return address_; }

    bool begin(uint32_t now) override;
    void poll(uint32_t now) override;
    void invalidateMetrics(ErrorCode code) override;

private:
    bool writeReg(uint8_t reg, uint8_t value);
    bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len);

    const char *name_ = "MPU";
    uint8_t address_ = 0x68;
    uint32_t lastPollMs_ = 0;
    uint32_t lastMotionMs_ = 0;
    bool filterReady_ = false;
    float pitch_ = 0.0f;
    float roll_ = 0.0f;
};
