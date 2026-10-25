// Faza 2: test OLED SSD1306 128×32 sa Adafruit SSD1306 bibliotekom
// Poređenje osvetljenja sa U8g2 (test_oled)

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "pins.h"

// Magistrala ostaje na 100 kHz i tokom i posle slanja na ekran
static Adafruit_SSD1306 oled(128, 32, &Wire, -1, 100000UL, 100000UL);

void setUp() {}
void tearDown() {}

static void showText(const char *line1, const char *line2) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 4);
    oled.print(line1);
    oled.setCursor(0, 20);
    oled.print(line2);
    oled.display();
}

static void maxBrightness() {
    oled.ssd1306_command(SSD1306_SETCONTRAST);
    oled.ssd1306_command(0xFF);
    oled.ssd1306_command(SSD1306_SETPRECHARGE);
    oled.ssd1306_command(0xF1);
    oled.ssd1306_command(SSD1306_SETVCOMDETECT);
    oled.ssd1306_command(0x40);
}

static void test_adafruit_begin() {
    // SWITCHCAPVCC: napon za ekran pravi interna pumpa punjenja (charge pump)
    TEST_ASSERT_TRUE_MESSAGE(oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false),
                             "Adafruit SSD1306: inicijalizacija nije uspela");
}

static void test_adafruit_brightness() {
    showText("Adafruit SSD1306", "podrazumevano");
    Serial.println("  1/4 Tekst, podrazumevano osvetljenje (5 s)");
    delay(5000);

    oled.fillScreen(SSD1306_WHITE);
    oled.display();
    Serial.println("  2/4 Ceo ekran beo, podrazumevano osvetljenje (5 s)");
    delay(5000);

    maxBrightness();
    Serial.println("  3/4 Ceo ekran beo, MAKSIMALNO osvetljenje (5 s)");
    delay(5000);

    showText("Adafruit SSD1306", "MAKSIMALNO");
    Serial.println("  4/4 Tekst, maksimalno osvetljenje (5 s)");
    delay(5000);
}

static void test_adafruit_charge_pump() {
    // Charge pump: 0x14 = 7,5 V (standard), 0x95 = 9,0 V (novije revizije SSD1306)
    oled.fillScreen(SSD1306_WHITE);
    oled.display();
    oled.ssd1306_command(SSD1306_CHARGEPUMP);
    oled.ssd1306_command(0x14);
    Serial.println("  5/6 Ceo ekran beo, pumpa punjenja 7,5 V (5 s)");
    delay(5000);

    oled.ssd1306_command(SSD1306_CHARGEPUMP);
    oled.ssd1306_command(0x95);
    Serial.println("  6/6 Ceo ekran beo, pumpa punjenja 9,0 V (5 s), zatim tekst");
    delay(5000);

    showText("Pumpa 9,0 V", "MAKSIMALNO");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    UNITY_BEGIN();
    RUN_TEST(test_adafruit_begin);
    RUN_TEST(test_adafruit_brightness);
    RUN_TEST(test_adafruit_charge_pump);
    UNITY_END();
}

void loop() {}
