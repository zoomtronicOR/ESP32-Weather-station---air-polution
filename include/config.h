#pragma once

// Osnovni parametri firmware-a

#define DEVICE_NAME       "esp32_r201"
#define FW_VERSION        "0.1.0"

#define SERIAL_BAUD       115200

// Task watchdog za glavnu petlju
#define WDT_TIMEOUT_MS    8000

// I2C magistrala: 100 kHz za početno testiranje (specifikacija §3.1)
#define I2C_FREQUENCY_HZ  100000

// Interval periodičnog ispisa statusa na serijski port
#define STATUS_INTERVAL_MS 10000
