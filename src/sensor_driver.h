#pragma once

#include "config.h"
#include "log.h"
#include "project_types.h"

// Zajednički interfejs drajvera senzora (specifikacija §4A.1)

class SensorDriver {
public:
    virtual ~SensorDriver() = default;

    virtual const char *id() const = 0;
    virtual const char *name() const = 0;  // prepoznata varijanta čipa
    virtual uint8_t bus() const = 0;       // 0 = glavna I2C magistrala, 1 = I2C1 (SPS30)
    virtual uint8_t address() const = 0;

    // Detekcija (uz potvrdu identiteta čipa) i inicijalizacija
    virtual bool begin(uint32_t now) = 0;
    // Neblokirajuće čitanje, poziva se iz glavne petlje
    virtual void poll(uint32_t now) = 0;
    // Označava sve metrike senzora kao nevalidne
    virtual void invalidateMetrics(ErrorCode code) = 0;

    SensorStatus status() const { return status_; }
    uint32_t errorCount() const { return errorCount_; }
    uint32_t lastOkMs() const { return lastOkMs_; }
    ErrorCode lastError() const { return lastError_; }

    void setNotFound() {
        status_ = SensorStatus::NotFound;
        lastError_ = ErrorCode::NotFound;
        invalidateMetrics(ErrorCode::NotFound);
    }

protected:
    void markOk(uint32_t now) {
        status_ = SensorStatus::Ok;
        lastOkMs_ = now;
        consecutiveErrors_ = 0;
        lastError_ = ErrorCode::None;
    }

    void markError(ErrorCode code) {
        errorCount_++;
        consecutiveErrors_++;
        lastError_ = code;
        if (consecutiveErrors_ >= SENSOR_MAX_ERRORS && status_ != SensorStatus::Error) {
            status_ = SensorStatus::Error;
            invalidateMetrics(code);
            LOG_W(id(), "%u uzastopnih grešaka (%s) -- senzor se ponovo inicijalizuje",
                  (unsigned)consecutiveErrors_, errorCodeText(code));
        }
    }

    SensorStatus status_ = SensorStatus::NotFound;
    uint32_t errorCount_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t lastOkMs_ = 0;
    ErrorCode lastError_ = ErrorCode::None;
};
