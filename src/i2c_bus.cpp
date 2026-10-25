#include "i2c_bus.h"

#include <Wire.h>
#include <esp_task_wdt.h>

#include "log.h"
#include "pins.h"

namespace i2c_bus {

static const char *TAG = "i2c";

static bool s_ready = false;

static bool checkIdleLines() {
    // Pre inicijalizacije obe linije moraju biti na visokom nivou (pull-up otpornici)
    pinMode(PIN_I2C_SDA, INPUT);
    pinMode(PIN_I2C_SCL, INPUT);
    delay(2);
    bool sda = digitalRead(PIN_I2C_SDA);
    bool scl = digitalRead(PIN_I2C_SCL);
    if (!sda || !scl) {
        LOG_E(TAG, "Linija %s%s%s je na niskom nivou -- nema pull-up otpornika ili postoji kratak spoj",
              !sda ? "SDA" : "", (!sda && !scl) ? " i " : "", !scl ? "SCL" : "");
        return false;
    }
    return true;
}

bool begin(uint32_t frequencyHz) {
    bool linesOk = checkIdleLines();
    s_ready = Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, frequencyHz);
    Wire.setTimeOut(50);
    if (s_ready) {
        LOG_I(TAG, "I2C magistrala: SDA GPIO%d, SCL GPIO%d, %lu kHz",
              PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)(frequencyHz / 1000));
    } else {
        LOG_E(TAG, "Inicijalizacija I2C magistrale nije uspela");
    }
    return s_ready && linesOk;
}

bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(address, (uint8_t)1) != 1) {
        return false;
    }
    value = Wire.read();
    return true;
}

static void identify(Device &dev) {
    uint8_t id = 0;
    dev.identified = false;

    switch (dev.address) {
        case 0x76:
        case 0x77:
            // Bosch: registar chip_id 0xD0
            if (readRegister(dev.address, 0xD0, id)) {
                dev.identified = true;
                if (id == 0x61)      dev.name = "BME680";
                else if (id == 0x60) dev.name = "BME280";
                else if (id == 0x58) dev.name = "BMP280";
                else if (id == 0x50) dev.name = "BMP388";
                else { dev.name = "Bosch senzor (nepoznat chip_id)"; dev.identified = false; }
            } else {
                dev.name = "BME680 / BME280 / BMP280 (?)";
            }
            break;

        case 0x68:
        case 0x69:
            // InvenSense MPU: registar WHO_AM_I 0x75
            if (readRegister(dev.address, 0x75, id)) {
                dev.identified = true;
                if (id == 0x71)      dev.name = "MPU9250";
                else if (id == 0x73) dev.name = "MPU9255";
                else if (id == 0x70) dev.name = "MPU6500";
                else if (id == 0x68) dev.name = "MPU6050 / MPU9150";
                else if (id == 0x74) dev.name = "MPU6515";
                else if (id == 0x75) dev.name = "MPU9250 klon (WHO_AM_I 0x75)";
                else { dev.name = "MPU / DS3231 (nepoznat WHO_AM_I)"; dev.identified = false; }
            } else {
                dev.name = "MPU / DS3231 (?)";
            }
            if (dev.identified) {
                LOG_D(TAG, "0x%02X WHO_AM_I = 0x%02X", dev.address, id);
            }
            break;

        case 0x53:
            // LTR-390: registar PART_ID 0x06, gornja 4 bita = 0xB
            if (readRegister(dev.address, 0x06, id) && (id >> 4) == 0x0B) {
                dev.name = "LTR-390";
                dev.identified = true;
            } else {
                dev.name = "LTR-390 / ENS160 (?)";
            }
            break;

        case 0x3C:
        case 0x3D: dev.name = "OLED SSD1306"; break;
        case 0x0C: dev.name = "AK8963 magnetometar (MPU9250)"; break;
        case 0x38: dev.name = "AHT20 / AHT21 / AHT25"; break;
        case 0x44:
        case 0x45: dev.name = "SHT3x / SHT4x"; break;
        case 0x52: dev.name = "ENS160"; break;
        case 0x62: dev.name = "SCD40 / SCD41"; break;
        case 0x29: dev.name = "TSL2591 / VL53L0X"; break;
        case 0x10: dev.name = "VEML7700"; break;
        case 0x01:
        case 0x02:
        case 0x03: dev.name = "AS3935"; break;
        case 0x70: dev.name = "TCA9548A multiplekser"; break;
        default:   dev.name = "nepoznat uređaj"; break;
    }
}

uint8_t scan(Device *out, uint8_t maxDevices) {
    if (!s_ready) {
        return 0;
    }
    uint8_t count = 0;
    for (uint8_t addr = 0x01; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0 && count < maxDevices) {
            out[count].address = addr;
            identify(out[count]);
            count++;
        }
    }
    return count;
}

static void stressAt(const Device *devices, uint8_t count, uint32_t frequencyHz, uint16_t iterations) {
    Wire.setClock(frequencyHz);
    uint32_t errors = 0;
    uint32_t start = millis();
    for (uint16_t i = 0; i < iterations; i++) {
        for (uint8_t d = 0; d < count; d++) {
            Wire.beginTransmission(devices[d].address);
            if (Wire.endTransmission() != 0) {
                errors++;
            }
        }
        if ((i & 0x3F) == 0) {
            esp_task_wdt_reset();
        }
    }
    uint32_t elapsed = millis() - start;
    Serial.printf("  %3lu kHz: %lu transakcija, grešaka %lu, trajanje %lu ms\n",
                  (unsigned long)(frequencyHz / 1000), (unsigned long)iterations * count,
                  (unsigned long)errors, (unsigned long)elapsed);
}

void stressTest(uint16_t iterations) {
    Device devices[MAX_DEVICES];
    uint8_t count = scan(devices, MAX_DEVICES);
    Serial.println();
    Serial.println("============ Test pouzdanosti I2C magistrale ============");
    if (count == 0) {
        Serial.println("Nema uređaja za test.");
    } else {
        stressAt(devices, count, 100000, iterations);
        stressAt(devices, count, 400000, iterations);
        Wire.setClock(100000);
        Serial.println("Frekvencija vraćena na 100 kHz.");
    }
    Serial.println("=========================================================");
    Serial.println();
}

void printScan() {
    Device devices[MAX_DEVICES];
    uint8_t count = scan(devices, MAX_DEVICES);

    Serial.println();
    Serial.println("================ I2C skeniranje ==================");
    if (!s_ready) {
        Serial.println("I2C magistrala nije inicijalizovana.");
    } else if (count == 0) {
        Serial.println("Nije pronađen nijedan uređaj. Proveriti SDA/SCL, napajanje i GND.");
    } else {
        for (uint8_t i = 0; i < count; i++) {
            Serial.printf("  0x%02X  %-34s %s\n", devices[i].address, devices[i].name,
                          devices[i].identified ? "(potvrđeno ID registrom)" : "");
        }
        Serial.printf("Ukupno uređaja: %u\n", count);
    }
    Serial.println("==================================================");
    Serial.println();
}

}  // namespace i2c_bus
