// Faza 2: samostalni test BME680 (temperatura, vlažnost, pritisak, gasni otpor)

#include <Adafruit_BME680.h>
#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "pins.h"

static Adafruit_BME680 bme(&Wire);

void setUp() {}
void tearDown() {}

static void test_bme680_begin() {
    TEST_ASSERT_TRUE_MESSAGE(bme.begin(0x77), "BME680 nije pronađen na adresi 0x77");
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);  // 320 °C, 150 ms
}

static void test_bme680_reading() {
    // Prvo očitavanje gasnog otpora je često nestabilno, pa se uzima nekoliko uzoraka
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE_MESSAGE(bme.performReading(), "Očitavanje BME680 nije uspelo");
        Serial.printf("  uzorak %d: T = %.2f °C, RH = %.1f %%, p = %.2f hPa, gas = %.1f kΩ\n", i + 1,
                      bme.temperature, bme.humidity, bme.pressure / 100.0, bme.gas_resistance / 1000.0);
        delay(1000);
    }

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(40.0, 20.0, bme.temperature, "Temperatura van opsega -20..60 °C");
    TEST_ASSERT_TRUE_MESSAGE(bme.humidity > 0.0 && bme.humidity <= 100.0, "Vlažnost van opsega 0..100 %");
    float p = bme.pressure / 100.0;
    TEST_ASSERT_TRUE_MESSAGE(p > 870.0 && p < 1085.0, "Pritisak van opsega 870..1085 hPa");
    TEST_ASSERT_TRUE_MESSAGE(bme.gas_resistance > 0, "Gasni otpor nije izmeren");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    UNITY_BEGIN();
    RUN_TEST(test_bme680_begin);
    RUN_TEST(test_bme680_reading);
    UNITY_END();
}

void loop() {}
