#pragma once

#include <Arduino.h>
#include <Wire.h>

// I2C magistrale: glavna (Wire: BME680, MPU, LTR-390, OLED) i I2C1 (Wire1: SPS30)
// Inicijalizacija, provera pull-up otpornika, skeniranje i prepoznavanje čipova

namespace i2c_bus {

constexpr uint8_t MAX_DEVICES = 16;

struct Device {
    uint8_t address;
    const char *name;   // prepoznat čip ili opis mogućih čipova na adresi
    bool identified;    // true ako je čip potvrđen čitanjem ID registra
};

// Inicijalizuje obe magistrale
bool begin();

bool ready(TwoWire &wire);
// Rezultat provere spoljnih pull-up otpornika pri inicijalizaciji
bool pullupsOk(TwoWire &wire);
bool probe(TwoWire &wire, uint8_t address);
bool readRegister(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t &value);

// Skenira magistralu; vraća broj pronađenih uređaja
uint8_t scan(TwoWire &wire, Device *out, uint8_t maxDevices);
void printScan();

// Test pouzdanosti glavne magistrale na 100 i 400 kHz
void stressTest(uint16_t iterations);

}  // namespace i2c_bus
