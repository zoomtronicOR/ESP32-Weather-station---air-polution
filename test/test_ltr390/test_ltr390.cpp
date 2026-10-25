// Faza 2: samostalni test LTR-390 (ambijentalno svetlo i UV)

#include <Adafruit_LTR390.h>
#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "pins.h"

static Adafruit_LTR390 ltr;

void setUp() {}
void tearDown() {}

static bool waitForData(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (ltr.newDataAvailable()) return true;
        delay(10);
    }
    return false;
}

static void test_ltr390_begin() {
    TEST_ASSERT_TRUE_MESSAGE(ltr.begin(&Wire), "LTR-390 nije pronađen na adresi 0x53");
}

static void test_ltr390_als() {
    ltr.setMode(LTR390_MODE_ALS);
    ltr.setGain(LTR390_GAIN_3);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);  // 100 ms
    delay(150);
    ltr.readALS();  // odbacuje se prvo očitavanje posle promene režima

    TEST_ASSERT_TRUE_MESSAGE(waitForData(1000), "LTR-390 ne daje nove ALS podatke");
    uint32_t als = ltr.readALS();
    // Lux prema datasheet-u: 0,6 × ALS / (pojačanje × faktor integracije); 18 bit = 100 ms → faktor 1
    float lux = 0.6f * als / 3.0f;
    Serial.printf("  ALS = %lu, osvetljenost ≈ %.1f lux\n", (unsigned long)als, lux);
    TEST_ASSERT_TRUE(als < (1UL << 18));
}

static void test_ltr390_uvs() {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_18);
    ltr.setResolution(LTR390_RESOLUTION_20BIT);  // 400 ms
    delay(450);
    ltr.readUVS();

    TEST_ASSERT_TRUE_MESSAGE(waitForData(2000), "LTR-390 ne daje nove UV podatke");
    uint32_t uvs = ltr.readUVS();
    // Orijentaciono: osetljivost 2300 impulsa po UV indeksu pri pojačanju 18 i 20 bita.
    // Konačni UV indeks zahteva kalibraciju (specifikacija §2.2).
    float uvi = uvs / 2300.0f;
    Serial.printf("  UVS = %lu, UV indeks ≈ %.2f (nekalibrisano)\n", (unsigned long)uvs, uvi);
    TEST_ASSERT_TRUE(uvs < (1UL << 20));
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    UNITY_BEGIN();
    RUN_TEST(test_ltr390_begin);
    RUN_TEST(test_ltr390_als);
    RUN_TEST(test_ltr390_uvs);
    UNITY_END();
}

void loop() {}
