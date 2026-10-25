#pragma once

#include <Arduino.h>

// Zajednička I2C magistrala: inicijalizacija, skeniranje i prepoznavanje čipova

namespace i2c_bus {

constexpr uint8_t MAX_DEVICES = 16;

struct Device {
    uint8_t address;
    const char *name;   // prepoznat čip ili opis mogućih čipova na adresi
    bool identified;    // true ako je čip potvrđen čitanjem ID registra
};

bool begin(uint32_t frequencyHz = 100000);

// Skenira magistralu; vraća broj pronađenih uređaja
uint8_t scan(Device *out, uint8_t maxDevices);
void printScan();

bool readRegister(uint8_t address, uint8_t reg, uint8_t &value);

// Test pouzdanosti: ponovljeno čitanje svih pronađenih uređaja na 100 i 400 kHz
void stressTest(uint16_t iterations);

}  // namespace i2c_bus
