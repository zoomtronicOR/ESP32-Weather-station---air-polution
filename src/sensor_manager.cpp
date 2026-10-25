#include "sensor_manager.h"

#include "config.h"
#include "log.h"
#include "sensors/bme680_sensor.h"
#include "sensors/ltr390_sensor.h"
#include "sensors/mpu_sensor.h"
#include "sensors/sps30_sensor.h"

namespace sensor_manager {

static const char *TAG = "senzori";

static Bme680Sensor s_bme680;
static MpuSensor s_mpu;
static Ltr390Sensor s_ltr390;
static Sps30Sensor s_sps30;

static SensorDriver *const s_drivers[] = {&s_bme680, &s_mpu, &s_ltr390, &s_sps30};
static constexpr size_t DRIVER_COUNT = sizeof(s_drivers) / sizeof(s_drivers[0]);

static uint32_t s_lastAttemptMs[DRIVER_COUNT] = {0};

static void tryBegin(size_t i, uint32_t now) {
    SensorDriver *d = s_drivers[i];
    s_lastAttemptMs[i] = now;
    if (d->begin(now)) {
        LOG_I(TAG, "%s: pronađen (%s), magistrala I2C%u, adresa 0x%02X", d->id(), d->name(), d->bus(),
              d->address());
    } else {
        d->setNotFound();
        LOG_W(TAG, "%s: nije pronađen, novi pokušaj za %lu s", d->id(),
              (unsigned long)(SENSOR_RETRY_MS / 1000));
    }
}

void begin() {
    uint32_t now = millis();
    for (size_t i = 0; i < DRIVER_COUNT; i++) {
        tryBegin(i, now);
    }
}

void poll(uint32_t now) {
    for (size_t i = 0; i < DRIVER_COUNT; i++) {
        SensorDriver *d = s_drivers[i];
        switch (d->status()) {
            case SensorStatus::Ok:
            case SensorStatus::Initializing:
                d->poll(now);
                break;
            case SensorStatus::NotFound:
            case SensorStatus::Error:
                if (now - s_lastAttemptMs[i] >= SENSOR_RETRY_MS) {
                    tryBegin(i, now);
                }
                break;
            case SensorStatus::Disabled:
                break;
        }
    }
}

size_t count() { return DRIVER_COUNT; }

const SensorDriver *driver(size_t index) { return index < DRIVER_COUNT ? s_drivers[index] : nullptr; }

Health health() {
    Health h = {0, 0, 0, 0};
    for (size_t i = 0; i < DRIVER_COUNT; i++) {
        switch (s_drivers[i]->status()) {
            case SensorStatus::Ok:           h.total++; h.ok++; break;
            case SensorStatus::Initializing: h.total++; h.initializing++; break;
            case SensorStatus::NotFound:
            case SensorStatus::Error:        h.total++; h.faulty++; break;
            case SensorStatus::Disabled:     break;
        }
    }
    return h;
}

void printStatus(uint32_t now) {
    Serial.println();
    Serial.println("================= Status senzora =================");
    for (size_t i = 0; i < DRIVER_COUNT; i++) {
        SensorDriver *d = s_drivers[i];
        Serial.printf("  %-7s %-22s I2C%u 0x%02X  %-16s grešaka %lu", d->id(), d->name(), d->bus(), d->address(),
                      sensorStatusText(d->status()), (unsigned long)d->errorCount());
        if (d->lastOkMs() > 0) {
            Serial.printf(", poslednje očitavanje pre %lu s", (unsigned long)((now - d->lastOkMs()) / 1000));
        }
        Serial.println();
    }
    Serial.println("==================================================");
    Serial.println();
}

}  // namespace sensor_manager
