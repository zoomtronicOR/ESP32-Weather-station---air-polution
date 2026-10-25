// Faza 3: samostalni test SPS30 na zasebnoj I2C magistrali (I2C1 / Wire1)

#include <Arduino.h>
#include <SensirionI2cSps30.h>
#include <Wire.h>
#include <unity.h>

#include "pins.h"

static SensirionI2cSps30 sps30;

void setUp() {}
void tearDown() {}

static void test_sps30_pullups() {
    // Pre inicijalizacije obe linije moraju biti na visokom nivou (pull-up na 3,3 V)
    pinMode(PIN_SPS30_SDA, INPUT);
    pinMode(PIN_SPS30_SCL, INPUT);
    delay(2);
    bool sda = digitalRead(PIN_SPS30_SDA);
    bool scl = digitalRead(PIN_SPS30_SCL);
    Serial.printf("  SDA (GPIO%d) = %s, SCL (GPIO%d) = %s\n", PIN_SPS30_SDA, sda ? "visok" : "NIZAK",
                  PIN_SPS30_SCL, scl ? "visok" : "NIZAK");
    TEST_ASSERT_TRUE_MESSAGE(sda && scl, "Linija na niskom nivou: nema pull-up otpornika ili je kratak spoj");
}

static void test_sps30_scan() {
    TEST_ASSERT_TRUE(Wire1.begin(PIN_SPS30_SDA, PIN_SPS30_SCL, 100000));
    Wire1.setTimeOut(100);
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire1.beginTransmission(addr);
        if (Wire1.endTransmission() == 0) {
            Serial.printf("  I2C1: uređaj na adresi 0x%02X\n", addr);
            found++;
        }
    }
    Wire1.beginTransmission(SPS30_I2C_ADDR_69);
    TEST_ASSERT_EQUAL_MESSAGE(0, Wire1.endTransmission(), "SPS30 ne odgovara na adresi 0x69 (proveriti SEL na GND)");
    TEST_ASSERT_EQUAL_MESSAGE(1, found, "Na I2C1 magistrali treba da bude samo SPS30");
}

static void test_sps30_info() {
    sps30.begin(Wire1, SPS30_I2C_ADDR_69);
    sps30.stopMeasurement();  // u slučaju da merenje već radi od ranije
    delay(100);

    int8_t serial[32] = {0};
    TEST_ASSERT_EQUAL_MESSAGE(0, sps30.readSerialNumber(serial, sizeof(serial)), "Čitanje serijskog broja nije uspelo");
    uint8_t major = 0, minor = 0;
    TEST_ASSERT_EQUAL_MESSAGE(0, sps30.readFirmwareVersion(major, minor), "Čitanje verzije firmware-a nije uspelo");
    uint32_t status = 0;
    TEST_ASSERT_EQUAL(0, sps30.readDeviceStatusRegister(status));

    Serial.printf("  Serijski broj: %s, firmware %u.%u, statusni registar 0x%08lX\n", (const char *)serial, major,
                  minor, (unsigned long)status);
}

static void test_sps30_measurement() {
    TEST_ASSERT_EQUAL_MESSAGE(0, sps30.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT),
                              "Pokretanje merenja nije uspelo");
    Serial.println("  Ventilator se pokreće, čeka se stabilizacija (oko 10 s)...");

    float mc1p0 = 0, mc2p5 = 0, mc4p0 = 0, mc10p0 = 0;
    float nc0p5, nc1p0, nc2p5, nc4p0, nc10p0, typicalSize = 0;
    int samples = 0;
    uint32_t start = millis();

    while (samples < 8 && millis() - start < 30000) {
        uint16_t ready = 0;
        if (sps30.readDataReadyFlag(ready) == 0 && ready) {
            TEST_ASSERT_EQUAL(0, sps30.readMeasurementValuesFloat(mc1p0, mc2p5, mc4p0, mc10p0, nc0p5, nc1p0, nc2p5,
                                                                  nc4p0, nc10p0, typicalSize));
            samples++;
            Serial.printf("  uzorak %d: PM1.0 = %.1f, PM2.5 = %.1f, PM4 = %.1f, PM10 = %.1f µg/m³, "
                          "tipična čestica %.2f µm\n",
                          samples, mc1p0, mc2p5, mc4p0, mc10p0, typicalSize);
        }
        delay(250);
    }

    uint32_t status = 0;
    sps30.readDeviceStatusRegister(status);
    sps30.stopMeasurement();
    Serial.printf("  Statusni registar posle merenja: 0x%08lX%s%s%s\n", (unsigned long)status,
                  (status & (1UL << 21)) ? " [upozorenje: brzina ventilatora]" : "",
                  (status & (1UL << 5)) ? " [greška lasera]" : "", (status & (1UL << 4)) ? " [greška ventilatora]" : "");

    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(5, samples, "SPS30 ne daje nova merenja");
    TEST_ASSERT_TRUE_MESSAGE(mc2p5 >= 0 && mc2p5 < 1000 && mc10p0 >= mc2p5, "PM vrednosti van očekivanog opsega");
    TEST_ASSERT_EQUAL_MESSAGE(0, status & ((1UL << 5) | (1UL << 4)), "SPS30 prijavljuje grešku lasera ili ventilatora");
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    UNITY_BEGIN();
    RUN_TEST(test_sps30_pullups);
    RUN_TEST(test_sps30_scan);
    RUN_TEST(test_sps30_info);
    RUN_TEST(test_sps30_measurement);
    UNITY_END();
}

void loop() {}
