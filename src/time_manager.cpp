#include "time_manager.h"

#include <esp_sntp.h>
#include <lwip/tcpip.h>
#include <sys/time.h>

#include "config.h"
#include "config_manager.h"
#include "log.h"

namespace time_manager {

static const char *TAG = "vreme";

static volatile bool s_syncEvent = false;
static volatile int64_t s_syncOffsetMs = 0;

static TimeStatus s_status = TimeStatus::NotSet;
static bool s_sntpRunning = false;
static time_t s_lastSync = 0;
static uint32_t s_lastSyncUptimeMs = 0;
static int32_t s_lastOffsetMs = 0;
static uint32_t s_syncCount = 0;
static uint32_t s_syncStartMs = 0;

// Poziva se iz SNTP zadatka -- ovde se samo beleži događaj, obrada je u update()
static void onTimeSync(struct timeval *tv) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    s_syncOffsetMs = ((int64_t)tv->tv_sec - now.tv_sec) * 1000 + ((int64_t)tv->tv_usec - now.tv_usec) / 1000;
    s_syncEvent = true;
}

static void applyTimezone() {
    const char *posix = config_manager::posixTimezone(config_manager::get().timezone);
    setenv("TZ", posix ? posix : "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

static void stopSntp() {
    if (s_sntpRunning) {
        // lwIP zahteva zaključavanje jezgra kada je uključeno CONFIG_LWIP_TCPIP_CORE_LOCKING
#if CONFIG_LWIP_TCPIP_CORE_LOCKING
        LOCK_TCPIP_CORE();
        esp_sntp_stop();
        UNLOCK_TCPIP_CORE();
#else
        esp_sntp_stop();
#endif
        s_sntpRunning = false;
    }
}

static void startSntp(uint32_t now) {
    const DeviceConfig &cfg = config_manager::get();
    sntp_set_sync_interval((uint32_t)cfg.ntpIntervalMin * 60UL * 1000UL);
    // Male korekcije postepeno, velike (> 35 min) odmah (specifikacija §12.3)
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    sntp_set_time_sync_notification_cb(onTimeSync);
    // configTzTime postavlja zonu i (ponovo) pokreće SNTP uz potrebno zaključavanje lwIP-a.
    // Nazivi servera se čuvaju kao pokazivači, pa moraju ostati trajni (statička konfiguracija).
    const char *posix = config_manager::posixTimezone(cfg.timezone);
    configTzTime(posix ? posix : "CET-1CEST,M3.5.0,M10.5.0/3", cfg.ntpServer1, cfg.ntpServer2, cfg.ntpServer3);
    s_sntpRunning = true;
    s_syncStartMs = now;
    if (s_status == TimeStatus::NotSet) {
        s_status = TimeStatus::Syncing;
    }
    LOG_I(TAG, "NTP pokrenut: %s, %s, %s (interval %u min)", cfg.ntpServer1, cfg.ntpServer2, cfg.ntpServer3,
          cfg.ntpIntervalMin);
}

void begin() { applyTimezone(); }

void applyConfig() {
    applyTimezone();
    if (s_sntpRunning) {
        startSntp(millis());
    }
}

void update(uint32_t now, bool networkUp) {
    const DeviceConfig &cfg = config_manager::get();

    if (networkUp && cfg.ntpEnabled && !s_sntpRunning) {
        startSntp(now);
    } else if ((!cfg.ntpEnabled) && s_sntpRunning) {
        stopSntp();
    }

    if (s_syncEvent) {
        s_syncEvent = false;
        s_lastOffsetMs = (int32_t)s_syncOffsetMs;
        s_lastSync = time(nullptr);
        s_lastSyncUptimeMs = now;
        s_syncCount++;
        bool first = s_status != TimeStatus::Synced && s_status != TimeStatus::Stale;
        s_status = TimeStatus::Synced;
        char buf[32];
        formatIsoLocal(s_lastSync, buf, sizeof(buf));
        if (first) {
            LOG_I(TAG, "Vreme sinhronizovano: %s %s", buf, tzAbbrev());
        } else {
            LOG_D(TAG, "NTP sinhronizacija, odstupanje %ld ms", (long)s_lastOffsetMs);
        }
    }

    if (s_status == TimeStatus::Synced && now - s_lastSyncUptimeMs > TIME_STALE_MS) {
        s_status = TimeStatus::Stale;
        LOG_W(TAG, "Poslednja NTP sinhronizacija je starija od 24 h");
    }

    if (s_status == TimeStatus::Syncing && s_sntpRunning && now - s_syncStartMs > TIME_SYNC_WARN_MS) {
        s_syncStartMs = now;  // upozorenje se ponavlja u istom intervalu
        LOG_W(TAG, "Vreme još nije sinhronizovano -- proveriti pristup internetu i NTP servere");
    }
}

TimeStatus status() { return s_status; }

const char *statusKey(TimeStatus s) {
    switch (s) {
        case TimeStatus::NotSet:  return "not_set";
        case TimeStatus::Syncing: return "syncing";
        case TimeStatus::Synced:  return "synced";
        case TimeStatus::Stale:   return "stale";
        case TimeStatus::Manual:  return "manual";
    }
    return "not_set";
}

bool isValid() {
    return s_status == TimeStatus::Synced || s_status == TimeStatus::Stale || s_status == TimeStatus::Manual;
}

time_t nowUtc() { return time(nullptr); }

time_t epochFromUptime(uint32_t uptimeMs) {
    return nowUtc() - (time_t)((millis() - uptimeMs) / 1000);
}

bool setManual(time_t epoch) {
    if (epoch < 1700000000) {  // pre novembra 2023 -- sigurno neispravno
        return false;
    }
    // Prva uspešna NTP sinhronizacija ima prednost nad ručnim vremenom (specifikacija §12.6)
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
    if (s_status != TimeStatus::Synced) {
        s_status = TimeStatus::Manual;
    }
    char buf[32];
    formatIsoLocal(epoch, buf, sizeof(buf));
    LOG_I(TAG, "Vreme ručno podešeno: %s", buf);
    return true;
}

time_t lastSyncUtc() { return s_lastSync; }

int32_t lastOffsetMs() { return s_lastOffsetMs; }

uint32_t syncCount() { return s_syncCount; }

void formatIsoUtc(time_t t, char *buf, size_t len) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void formatIsoLocal(time_t t, char *buf, size_t len) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    char tz[8];
    strftime(tz, sizeof(tz), "%z", &tmv);  // +0100
    char base[24];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(buf, len, "%s%c%c%c:%c%c", base, tz[0], tz[1], tz[2], tz[3], tz[4]);
}

const char *tzAbbrev() {
    time_t t = nowUtc();
    struct tm tmv;
    localtime_r(&t, &tmv);
    return tmv.tm_isdst > 0 ? tzname[1] : tzname[0];
}

bool formatClock(char *timeBuf, size_t tlen, char *dateBuf, size_t dlen) {
    if (!isValid()) {
        return false;
    }
    time_t t = nowUtc();
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(timeBuf, tlen, "%H:%M", &tmv);
    strftime(dateBuf, dlen, "%d.%m.", &tmv);
    return true;
}

}  // namespace time_manager
