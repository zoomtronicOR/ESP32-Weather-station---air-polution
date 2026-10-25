// Faza 2: samostalni test OLED SSD1306 128×32 (U8g2, srpska latinica)

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <unity.h>

#include "pins.h"

static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);

void setUp() {}
void tearDown() {}

static void test_oled_ack() {
    Wire.beginTransmission(0x3C);
    TEST_ASSERT_EQUAL_MESSAGE(0, Wire.endTransmission(), "OLED ne odgovara na adresi 0x3C");
}

static void test_oled_draw() {
    oled.setI2CAddress(0x3C << 1);
    oled.setBusClock(100000);  // U8g2 inače prebacuje magistralu na 400 kHz
    TEST_ASSERT_TRUE_MESSAGE(oled.begin(), "Inicijalizacija OLED-a nije uspela");
    oled.enableUTF8Print();

    oled.clearBuffer();
    oled.setFont(u8g2_font_6x13_te);  // sadrži č ć š ž đ
    oled.setCursor(0, 12);
    oled.print("Meteo stanica");
    oled.setCursor(0, 28);
    oled.print("Test: čćšžđ ČĆŠŽĐ");
    oled.sendBuffer();
    Serial.println("  Na ekranu treba da piše: 'Meteo stanica' i 'Test: čćšžđ ČĆŠŽĐ'");
    delay(3000);

    // Okvir uz ivice: provera da je ceo ekran vidljiv (128×32)
    oled.clearBuffer();
    oled.drawFrame(0, 0, 128, 32);
    oled.setCursor(4, 20);
    oled.print("Okvir 128×32");
    oled.sendBuffer();
    Serial.println("  Zatim: okvir uz sve četiri ivice i tekst 'Okvir 128×32'");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    UNITY_BEGIN();
    RUN_TEST(test_oled_ack);
    RUN_TEST(test_oled_draw);
    UNITY_END();
}

void loop() {}
