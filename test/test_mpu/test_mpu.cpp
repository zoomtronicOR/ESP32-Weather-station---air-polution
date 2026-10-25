// Faza 2: samostalni test MPU6500 (akcelerometar, žiroskop) preko registara

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <unity.h>

#include "pins.h"

static const uint8_t MPU_ADDR = 0x68;

static const uint8_t REG_PWR_MGMT_1   = 0x6B;
static const uint8_t REG_ACCEL_CONFIG = 0x1C;
static const uint8_t REG_GYRO_CONFIG  = 0x1B;
static const uint8_t REG_ACCEL_XOUT_H = 0x3B;
static const uint8_t REG_WHO_AM_I     = 0x75;

// ±2 g i ±250 °/s
static const float ACCEL_LSB_PER_G   = 16384.0f;
static const float GYRO_LSB_PER_DPS  = 131.0f;

static bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(MPU_ADDR, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

void setUp() {}
void tearDown() {}

static void test_mpu_who_am_i() {
    uint8_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(readRegs(REG_WHO_AM_I, &id, 1), "MPU ne odgovara na adresi 0x68");
    Serial.printf("  WHO_AM_I = 0x%02X\n", id);
    TEST_ASSERT_TRUE_MESSAGE(id == 0x70 || id == 0x71 || id == 0x73, "Neočekivan WHO_AM_I");
}

static void test_mpu_wake() {
    TEST_ASSERT_TRUE(writeReg(REG_PWR_MGMT_1, 0x80));  // reset
    delay(100);
    TEST_ASSERT_TRUE(writeReg(REG_PWR_MGMT_1, 0x01));  // buđenje, PLL takt
    delay(50);
    TEST_ASSERT_TRUE(writeReg(REG_ACCEL_CONFIG, 0x00));
    TEST_ASSERT_TRUE(writeReg(REG_GYRO_CONFIG, 0x00));
    delay(50);
}

static void test_mpu_reading() {
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0, temp = 0;
    const int samples = 20;

    for (int i = 0; i < samples; i++) {
        uint8_t b[14];
        TEST_ASSERT_TRUE_MESSAGE(readRegs(REG_ACCEL_XOUT_H, b, 14), "Čitanje podataka MPU nije uspelo");
        ax += (int16_t)(b[0] << 8 | b[1]) / ACCEL_LSB_PER_G;
        ay += (int16_t)(b[2] << 8 | b[3]) / ACCEL_LSB_PER_G;
        az += (int16_t)(b[4] << 8 | b[5]) / ACCEL_LSB_PER_G;
        temp += (int16_t)(b[6] << 8 | b[7]) / 333.87f + 21.0f;  // MPU6500
        gx += (int16_t)(b[8] << 8 | b[9]) / GYRO_LSB_PER_DPS;
        gy += (int16_t)(b[10] << 8 | b[11]) / GYRO_LSB_PER_DPS;
        gz += (int16_t)(b[12] << 8 | b[13]) / GYRO_LSB_PER_DPS;
        delay(20);
    }
    ax /= samples; ay /= samples; az /= samples;
    gx /= samples; gy /= samples; gz /= samples;
    temp /= samples;

    float magnitude = sqrtf(ax * ax + ay * ay + az * az);
    float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;
    float roll = atan2f(ay, az) * 180.0f / PI;

    Serial.printf("  ubrzanje: x = %.3f g, y = %.3f g, z = %.3f g, |a| = %.3f g\n", ax, ay, az, magnitude);
    Serial.printf("  žiroskop: x = %.2f °/s, y = %.2f °/s, z = %.2f °/s\n", gx, gy, gz);
    Serial.printf("  nagib: pitch = %.1f °, roll = %.1f °, temperatura čipa = %.1f °C\n", pitch, roll, temp);

    // Ploča miruje: ukupno ubrzanje ≈ 1 g, ugaona brzina ≈ 0
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.15f, 1.0f, magnitude, "Ukupno ubrzanje nije ≈ 1 g (ploča mora mirovati)");
    TEST_ASSERT_TRUE_MESSAGE(fabsf(gx) < 10 && fabsf(gy) < 10 && fabsf(gz) < 10,
                             "Žiroskop pokazuje rotaciju veću od 10 °/s u mirovanju");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    UNITY_BEGIN();
    RUN_TEST(test_mpu_who_am_i);
    RUN_TEST(test_mpu_wake);
    RUN_TEST(test_mpu_reading);
    UNITY_END();
}

void loop() {}
