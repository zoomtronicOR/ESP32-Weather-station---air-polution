#include "ota_manager.h"

#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "log.h"

// Arduino jezgro inače odmah potvrđuje novi firmware; ovde se potvrda odlaže dok
// uređaj ne radi stabilno OTA_VERIFY_MS (specifikacija §15, povratak na prethodnu verziju)
extern "C" bool verifyRollbackLater() { return true; }

namespace ota_manager {

static const char *TAG = "ota";

static bool s_started = false;
static volatile bool s_inProgress = false;
static volatile uint8_t s_progress = 0;
static const char *s_target = "firmware";
static bool s_pendingVerify = false;
static uint8_t s_lastShownProgress = 255;

static void showProgress(uint8_t pct) {
    // ArduinoOTA blokira glavnu petlju tokom prenosa, pa se ekran crta direktno
    if (pct != s_lastShownProgress && (pct % 5 == 0 || pct == 100)) {
        s_lastShownProgress = pct;
        display_manager::showOtaProgress(pct, s_target);
    }
}

static void setupArduinoOta() {
    const DeviceConfig &cfg = config_manager::get();
    ArduinoOTA.setHostname(cfg.hostname);
    ArduinoOTA.setPassword(config_manager::adminPassword());
    ArduinoOTA.setPartitionLabel("spiffs");  // web fajlovi; istorija je na posebnoj particiji
    ArduinoOTA.setMdnsEnabled(false);        // mDNS već objavljuje wifi_manager

    ArduinoOTA.onStart([]() {
        s_inProgress = true;
        s_progress = 0;
        s_lastShownProgress = 255;
        s_target = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "web fajlovi";
        if (ArduinoOTA.getCommand() != U_FLASH) LittleFS.end();
        LOG_I(TAG, "OTA ažuriranje (%s) je počelo", s_target);
        showProgress(0);
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        esp_task_wdt_reset();  // prenos traje duže od watchdog roka
        s_progress = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
        showProgress(s_progress);
    });
    ArduinoOTA.onEnd([]() {
        s_progress = 100;
        showProgress(100);
        LOG_I(TAG, "OTA ažuriranje (%s) je završeno -- restart", s_target);
    });
    ArduinoOTA.onError([](ota_error_t err) {
        s_inProgress = false;
        const char *why = err == OTA_AUTH_ERROR      ? "pogrešna lozinka"
                          : err == OTA_BEGIN_ERROR   ? "nije moguće započeti upis"
                          : err == OTA_CONNECT_ERROR ? "greška veze"
                          : err == OTA_RECEIVE_ERROR ? "greška prijema"
                                                     : "greška pri završetku (neispravna slika)";
        LOG_E(TAG, "OTA ažuriranje nije uspelo: %s", why);
    });
    ArduinoOTA.begin();
    s_started = true;
    LOG_I(TAG, "OTA spreman (port 3232)");
}

void begin() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pendingVerify = true;
        LOG_W(TAG, "Novi firmware (%s) čeka potvrdu -- potvrđuje se posle %lu s stabilnog rada", running->label,
              (unsigned long)(OTA_VERIFY_MS / 1000));
    }
}

void applyConfig() {
    if (!s_started) return;
    ArduinoOTA.end();
    s_started = false;
}

void update(uint32_t now, bool networkUp) {
    if (s_pendingVerify && now >= OTA_VERIFY_MS) {
        s_pendingVerify = false;
        esp_ota_mark_app_valid_cancel_rollback();
        LOG_I(TAG, "Novi firmware je potvrđen (rollback više nije moguć)");
    }
    if (!s_started && networkUp) {
        setupArduinoOta();
    }
    if (s_started) {
        ArduinoOTA.handle();
    }
}

bool inProgress() { return s_inProgress; }

uint8_t progress() { return s_progress; }

const char *target() { return s_target; }

// ---------------------------------------------------------------------------
// Upload iz web interfejsa
// ---------------------------------------------------------------------------

bool webBegin(bool filesystem, size_t size, String &error) {
    if (s_inProgress) {
        error = "Ažuriranje je već u toku.";
        return false;
    }
    s_target = filesystem ? "web fajlovi" : "firmware";
    if (filesystem) LittleFS.end();  // particija web fajlova se prepisuje
    bool ok = filesystem ? Update.begin(size ? size : UPDATE_SIZE_UNKNOWN, U_SPIFFS, -1, LOW, "spiffs")
                         : Update.begin(size ? size : UPDATE_SIZE_UNKNOWN, U_FLASH);
    if (!ok) {
        error = String("Upis nije moguće započeti: ") + Update.errorString();
        if (filesystem) LittleFS.begin(true);
        return false;
    }
    s_inProgress = true;
    s_progress = 0;
    LOG_I(TAG, "Ažuriranje iz web interfejsa (%s) je počelo", s_target);
    return true;
}

bool webWrite(uint8_t *data, size_t len, size_t total) {
    if (Update.write(data, len) != len) {
        LOG_E(TAG, "Greška pri upisu: %s", Update.errorString());
        return false;
    }
    if (total) s_progress = (uint8_t)min<uint64_t>(99, (uint64_t)Update.progress() * 100 / total);
    return true;
}

bool webEnd(String &error) {
    // end(true) proverava sliku (zaglavlje, čip, kontrolni zbir) pre nego što je označi za pokretanje
    if (!Update.end(true)) {
        error = String("Neispravna ili nepotpuna slika: ") + Update.errorString();
        LOG_E(TAG, "Ažuriranje nije uspelo: %s", Update.errorString());
        s_inProgress = false;
        return false;
    }
    s_progress = 100;
    LOG_I(TAG, "Ažuriranje (%s) je uspešno -- restart", s_target);
    return true;
}

void webAbort() {
    if (Update.isRunning()) Update.abort();
    s_inProgress = false;
}

const char *runningPartition() {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

bool pendingVerify() { return s_pendingVerify; }

}  // namespace ota_manager
