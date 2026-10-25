#include "sensors/mpu_sensor.h"

#include <Wire.h>
#include <math.h>

#include "i2c_bus.h"
#include "metrics.h"

static constexpr uint8_t REG_CONFIG        = 0x1A;
static constexpr uint8_t REG_GYRO_CONFIG   = 0x1B;
static constexpr uint8_t REG_ACCEL_CONFIG  = 0x1C;
static constexpr uint8_t REG_ACCEL_CONFIG2 = 0x1D;
static constexpr uint8_t REG_ACCEL_XOUT_H  = 0x3B;
static constexpr uint8_t REG_PWR_MGMT_1    = 0x6B;
static constexpr uint8_t REG_WHO_AM_I      = 0x75;

// ±2 g i ±250 °/s
static constexpr float ACCEL_LSB_PER_G  = 16384.0f;
static constexpr float GYRO_LSB_PER_DPS = 131.0f;

// Niskopropusni filter za nagib (0..1, manje = sporije i mirnije)
static constexpr float TILT_FILTER_ALPHA = 0.2f;

bool MpuSensor::writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool MpuSensor::readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address_, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool MpuSensor::begin(uint32_t now) {
    const uint8_t candidates[] = {0x68, 0x69};
    bool found = false;
    for (uint8_t addr : candidates) {
        uint8_t who = 0;
        if (!i2c_bus::readRegister(Wire, addr, REG_WHO_AM_I, who)) continue;
        if (who == 0x70)      name_ = "MPU6500";
        else if (who == 0x71) name_ = "MPU9250";
        else if (who == 0x73) name_ = "MPU9255";
        else continue;
        address_ = addr;
        found = true;
        break;
    }
    if (!found) {
        return false;
    }

    // Reset i podešavanje (jednokratno pri inicijalizaciji)
    if (!writeReg(REG_PWR_MGMT_1, 0x80)) return false;
    delay(100);
    bool ok = writeReg(REG_PWR_MGMT_1, 0x01)        // buđenje, PLL takt
              && writeReg(REG_CONFIG, 0x03)         // DLPF žiroskopa 41 Hz
              && writeReg(REG_GYRO_CONFIG, 0x00)    // ±250 °/s
              && writeReg(REG_ACCEL_CONFIG, 0x00)   // ±2 g
              && writeReg(REG_ACCEL_CONFIG2, 0x03); // DLPF akcelerometra 41 Hz
    if (!ok) {
        return false;
    }

    filterReady_ = false;
    lastPollMs_ = now;
    lastMotionMs_ = 0;
    consecutiveErrors_ = 0;
    status_ = SensorStatus::Initializing;
    return true;
}

void MpuSensor::poll(uint32_t now) {
    if (now - lastPollMs_ < MPU_INTERVAL_MS) {
        return;
    }
    lastPollMs_ = now;

    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, sizeof(b))) {
        markError(ErrorCode::Comm);
        return;
    }

    float ax = (int16_t)(b[0] << 8 | b[1]) / ACCEL_LSB_PER_G;
    float ay = (int16_t)(b[2] << 8 | b[3]) / ACCEL_LSB_PER_G;
    float az = (int16_t)(b[4] << 8 | b[5]) / ACCEL_LSB_PER_G;
    float gx = (int16_t)(b[8] << 8 | b[9]) / GYRO_LSB_PER_DPS;
    float gy = (int16_t)(b[10] << 8 | b[11]) / GYRO_LSB_PER_DPS;
    float gz = (int16_t)(b[12] << 8 | b[13]) / GYRO_LSB_PER_DPS;

    float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;
    float roll = atan2f(ay, az) * 180.0f / PI;
    if (!filterReady_) {
        pitch_ = pitch;
        roll_ = roll;
        filterReady_ = true;
    } else {
        pitch_ += TILT_FILTER_ALPHA * (pitch - pitch_);
        roll_ += TILT_FILTER_ALPHA * (roll - roll_);
    }

    float accelMag = sqrtf(ax * ax + ay * ay + az * az);
    float gyroMag = sqrtf(gx * gx + gy * gy + gz * gz);
    if (gyroMag > MPU_MOTION_GYRO_DPS || fabsf(accelMag - 1.0f) > MPU_MOTION_ACCEL_G) {
        lastMotionMs_ = now;
    }
    bool moving = lastMotionMs_ != 0 && (now - lastMotionMs_) < MPU_MOTION_HOLD_MS;

    metrics::update(MetricId::Pitch, pitch_, now);
    metrics::update(MetricId::Roll, roll_, now);
    metrics::update(MetricId::Motion, moving ? 1.0f : 0.0f, now);
    markOk(now);
}

void MpuSensor::invalidateMetrics(ErrorCode code) {
    metrics::invalidate(MetricId::Pitch, code);
    metrics::invalidate(MetricId::Roll, code);
    metrics::invalidate(MetricId::Motion, code);
}
