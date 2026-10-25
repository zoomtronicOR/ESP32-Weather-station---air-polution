#pragma once

// Raspored GPIO pinova -- NodeMCU ESP-32S V1.1 (ESP32-D0WD-V3)
// Ne koristiti GPIO6–11 (interna flash memorija).
// GPIO0, 2, 5, 12, 15 su strapping pinovi; GPIO12 mora biti nizak pri pokretanju.
// GPIO34–39 su samo ulazni, bez internih pull-up otpornika.

// Ugrađena LED dioda na ploči (status / heartbeat) -- potvrditi u Fazi 1
#define PIN_STATUS_LED    2

// Taster BOOT na ploči (čitati tek nakon pokretanja)
#define PIN_BOOT_BUTTON   0

// I2C magistrala (BME680, MPU, LTR-390, OLED)
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22

// SPS30 preko UART2
#define PIN_SPS30_RX      16
#define PIN_SPS30_TX      17

// WS2812B LED indikator kvaliteta vazduha (RMT)
#define PIN_WS2812B       27
