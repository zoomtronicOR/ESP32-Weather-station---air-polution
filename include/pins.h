#pragma once

// Raspored GPIO pinova -- NodeMCU ESP-32S V1.1 (ESP32-D0WD-V3)
// Ne koristiti GPIO6–11 (interna flash memorija).
// GPIO0, 2, 5, 12, 15 su strapping pinovi; GPIO12 mora biti nizak pri pokretanju.
// GPIO34–39 su samo ulazni, bez internih pull-up otpornika.

// Ugrađena plava LED dioda na ploči (status / heartbeat) -- potvrđeno u Fazi 1
#define PIN_STATUS_LED    2

// Taster BOOT na ploči (čitati tek nakon pokretanja)
#define PIN_BOOT_BUTTON   0

// I2C magistrala (BME680, MPU, LTR-390, OLED)
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22

// SPS30 na zasebnoj I2C magistrali (I2C1 / Wire1), 100 kHz, adresa 0x69
// SEL pin SPS30 na GND; pull-up otpornici na 3,3 V
#define PIN_SPS30_SDA     16
#define PIN_SPS30_SCL     17

// WS2812B LED indikator kvaliteta vazduha (RMT)
#define PIN_WS2812B       27

// ---- Rezervisano za opcione komponente (specifikacija §2.4, §3.4) ----

// AS3935 detektor munja
#define PIN_AS3935_IRQ    34   // samo ulaz
#define PIN_AS3935_CS     13   // samo ako se koristi SPI (VSPI)
#define PIN_AS3935_SCK    18
#define PIN_AS3935_MISO   19
#define PIN_AS3935_MOSI   23

// Anemometar i kišomer (PCNT, spoljni pull-up 10 kΩ)
#define PIN_ANEMOMETER    26
#define PIN_RAIN_GAUGE    25

// Analogni ulazi -- samo ADC1, jer ADC2 ne radi uz Wi-Fi
#define PIN_WIND_VANE     36   // SVP
#define PIN_RAIN_SENSOR   39   // SVN
#define PIN_RAIN_SENSOR_PWR 32

// MH-Z19B preko UART1
#define PIN_MHZ19_RX      35   // samo ulaz
#define PIN_MHZ19_TX      33

// Buzzer (preko tranzistora)
#define PIN_BUZZER        4
