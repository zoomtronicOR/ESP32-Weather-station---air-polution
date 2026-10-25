#include "i2c_bus.h"

#include <esp_task_wdt.h>

#include "config.h"
#include "log.h"
#include "pins.h"

namespace i2c_bus {

static const char *TAG = "i2c";

static bool s_ready[2] = {false, false};
static bool s_pullupsOk[2] = {false, false};

static uint8_t busIndex(TwoWire &wire) { return &wire == &Wire1 ? 1 : 0; }

static bool checkIdleLines(int sda, int scl, const char *busName) {
    // Pre inicijalizacije obe linije moraju biti na visokom nivou (pull-up otpornici)
    pinMode(sda, INPUT);
    pinMode(scl, INPUT);
    delay(2);
    bool sdaHigh = digitalRead(sda);
    bool sclHigh = digitalRead(scl);
    if (!sdaHigh || !sclHigh) {
        const char *lines = (!sdaHigh && !sclHigh) ? "linije SDA i SCL su" : (!sdaHigh ? "linija SDA je" : "linija SCL je");
        LOG_E(TAG, "%s: %s na niskom nivou -- nema spoljnih pull-up otpornika ili postoji kratak spoj", busName, lines);
        return false;
    }
    return true;
}

static bool beginBus(TwoWire &wire, int sda, int scl, uint32_t frequencyHz, const char *busName) {
    bool linesOk = checkIdleLines(sda, scl, busName);
    bool ok = wire.begin(sda, scl, frequencyHz);
    wire.setTimeOut(50);
    s_ready[busIndex(wire)] = ok;
    s_pullupsOk[busIndex(wire)] = linesOk;
    if (ok) {
        LOG_I(TAG, "%s: SDA GPIO%d, SCL GPIO%d, %lu kHz", busName, sda, scl, (unsigned long)(frequencyHz / 1000));
    } else {
        LOG_E(TAG, "%s: inicijalizacija nije uspela", busName);
    }
    return ok && linesOk;
}

bool begin() {
    bool main = beginBus(Wire, PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQUENCY_HZ, "I2C0 (glavna)");
    bool sps = beginBus(Wire1, PIN_SPS30_SDA, PIN_SPS30_SCL, I2C1_FREQUENCY_HZ, "I2C1 (SPS30)");
    return main && sps;
}

bool ready(TwoWire &wire) { return s_ready[busIndex(wire)]; }

bool pullupsOk(TwoWire &wire) { return s_pullupsOk[busIndex(wire)]; }

bool probe(TwoWire &wire, uint8_t address) {
    if (!ready(wire)) return false;
    wire.beginTransmission(address);
    return wire.endTransmission() == 0;
}

bool readRegister(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t &value) {
    wire.beginTransmission(address);
    wire.write(reg);
    if (wire.endTransmission(false) != 0) {
        return false;
    }
    if (wire.requestFrom(address, (uint8_t)1) != 1) {
        return false;
    }
    value = wire.read();
    return true;
}

static void identify(TwoWire &wire, Device &dev) {
    uint8_t id = 0;
    dev.identified = false;

    if (busIndex(wire) == 1) {
        dev.name = dev.address == 0x69 ? "SPS30" : "nepoznat uređaj";
        return;
    }

    switch (dev.address) {
        case 0x76:
        case 0x77:
            // Bosch: registar chip_id 0xD0
            if (readRegister(wire, dev.address, 0xD0, id)) {
                dev.identified = true;
                if (id == 0x61)      dev.name = "BME680";
                else if (id == 0x60) dev.name = "BME280";
                else if (id == 0x58) dev.name = "BMP280";
                else if (id == 0x50) dev.name = "BMP388";
                else { dev.name = "Bosch senzor (nepoznat chip_id)"; dev.identified = false; }
            } else {
                dev.name = "BME680 (?)";
            }
            break;

        case 0x68:
        case 0x69:
            // InvenSense MPU: registar WHO_AM_I 0x75
            if (readRegister(wire, dev.address, 0x75, id)) {
                dev.identified = true;
                if (id == 0x71)      dev.name = "MPU9250";
                else if (id == 0x73) dev.name = "MPU9255";
                else if (id == 0x70) dev.name = "MPU6500";
                else if (id == 0x68) dev.name = "MPU6050 / MPU9150";
                else { dev.name = "nepoznat (WHO_AM_I)"; dev.identified = false; }
            } else {
                dev.name = "MPU (?)";
            }
            break;

        case 0x53:
            // LTR-390: registar PART_ID 0x06, gornja 4 bita = 0xB
            if (readRegister(wire, dev.address, 0x06, id) && (id >> 4) == 0x0B) {
                dev.name = "LTR-390";
                dev.identified = true;
            } else {
                dev.name = "LTR-390 (?)";
            }
            break;

        case 0x3C:
        case 0x3D: dev.name = "OLED SSD1306"; break;
        case 0x0C: dev.name = "AK8963 magnetometar (MPU9250)"; break;
        case 0x38: dev.name = "AHT20 / AHT21 / AHT25"; break;
        case 0x44:
        case 0x45: dev.name = "SHT4x"; break;
        case 0x59: dev.name = "SGP40 / SGP41"; break;
        case 0x62: dev.name = "SCD40 / SCD41"; break;
        case 0x10: dev.name = "VEML7700"; break;
        case 0x23: dev.name = "BH1750"; break;
        case 0x01:
        case 0x02:
        case 0x03: dev.name = "AS3935"; break;
        default:
            dev.name = (dev.address >= 0x40 && dev.address <= 0x4F) ? "INA219 / INA226" : "nepoznat uređaj";
            break;
    }
}

uint8_t scan(TwoWire &wire, Device *out, uint8_t maxDevices) {
    if (!ready(wire)) {
        return 0;
    }
    uint8_t count = 0;
    for (uint8_t addr = 0x01; addr <= 0x77; addr++) {
        wire.beginTransmission(addr);
        if (wire.endTransmission() == 0 && count < maxDevices) {
            out[count].address = addr;
            identify(wire, out[count]);
            count++;
        }
    }
    return count;
}

static void printBus(TwoWire &wire, const char *busName) {
    Device devices[MAX_DEVICES];
    uint8_t count = scan(wire, devices, MAX_DEVICES);

    Serial.printf("%s:\n", busName);
    if (!ready(wire)) {
        Serial.println("  magistrala nije inicijalizovana");
    } else if (count == 0) {
        Serial.println("  nije pronađen nijedan uređaj -- proveriti SDA/SCL, napajanje i GND");
    } else {
        for (uint8_t i = 0; i < count; i++) {
            Serial.printf("  0x%02X  %-34s %s\n", devices[i].address, devices[i].name,
                          devices[i].identified ? "(potvrđeno ID registrom)" : "");
        }
    }
}

void printScan() {
    Serial.println();
    Serial.println("================ I2C skeniranje ==================");
    printBus(Wire, "I2C0 (glavna magistrala)");
    printBus(Wire1, "I2C1 (SPS30)");
    Serial.println("==================================================");
    Serial.println();
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
    uint8_t count = scan(Wire, devices, MAX_DEVICES);
    Serial.println();
    Serial.println("======== Test pouzdanosti glavne I2C magistrale ========");
    if (count == 0) {
        Serial.println("Nema uređaja za test.");
    } else {
        stressAt(devices, count, 100000, iterations);
        stressAt(devices, count, 400000, iterations);
        Wire.setClock(I2C_FREQUENCY_HZ);
        Serial.printf("Frekvencija vraćena na %lu kHz.\n", (unsigned long)(I2C_FREQUENCY_HZ / 1000));
    }
    Serial.println("=========================================================");
    Serial.println();
}

}  // namespace i2c_bus
