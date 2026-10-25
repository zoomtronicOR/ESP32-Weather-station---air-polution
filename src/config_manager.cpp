#include "config_manager.h"

#include <Preferences.h>

#include "log.h"

namespace config_manager {

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "cfg";
static constexpr uint32_t CONFIG_VERSION = 1;

static DeviceConfig s_config;
static char s_defaultApPass[16] = "";

struct TimezoneDef {
    const char *name;
    const char *posix;
};

// ESP32 nema bazu vremenskih zona -- naziv se prevodi u POSIX TZ string (specifikacija §12.2)
static const TimezoneDef TIMEZONES[] = {
    {"Europe/Belgrade", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London",   "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Athens",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Moscow",   "MSK-3"},
    {"UTC",             "UTC0"},
};

static void loadString(Preferences &p, const char *key, char *dst, size_t len) {
    String v = p.getString(key, dst);
    strlcpy(dst, v.c_str(), len);
}

void begin() {
    // Podrazumevana lozinka AP-a: "meteo" + poslednja 3 bajta MAC adrese (11 znakova)
    uint64_t mac = ESP.getEfuseMac();
    snprintf(s_defaultApPass, sizeof(s_defaultApPass), "meteo%02x%02x%02x", (uint8_t)(mac >> 24),
             (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));

    Preferences p;
    if (!p.begin(NVS_NAMESPACE, true)) {
        LOG_W(TAG, "Konfiguracija ne postoji -- koriste se podrazumevane vrednosti");
        return;
    }
    if (p.getUInt("ver", 0) != CONFIG_VERSION) {
        LOG_W(TAG, "Nepoznata verzija konfiguracije -- koriste se podrazumevane vrednosti");
        p.end();
        return;
    }
    loadString(p, "ssid", s_config.wifiSsid, sizeof(s_config.wifiSsid));
    loadString(p, "wpass", s_config.wifiPass, sizeof(s_config.wifiPass));
    s_config.staticIp = p.getBool("static", false);
    s_config.ip = p.getUInt("ip", 0);
    s_config.gateway = p.getUInt("gw", 0);
    s_config.subnet = p.getUInt("mask", 0);
    s_config.dns = p.getUInt("dns", 0);
    loadString(p, "host", s_config.hostname, sizeof(s_config.hostname));
    loadString(p, "admin", s_config.adminPass, sizeof(s_config.adminPass));
    loadString(p, "appass", s_config.apPass, sizeof(s_config.apPass));
    s_config.ntpEnabled = p.getBool("ntp", true);
    loadString(p, "ntp1", s_config.ntpServer1, sizeof(s_config.ntpServer1));
    loadString(p, "ntp2", s_config.ntpServer2, sizeof(s_config.ntpServer2));
    loadString(p, "ntp3", s_config.ntpServer3, sizeof(s_config.ntpServer3));
    s_config.ntpIntervalMin = p.getUShort("ntpint", 60);
    loadString(p, "tz", s_config.timezone, sizeof(s_config.timezone));
    s_config.oledBrightness = p.getUChar("oledbr", 80);
    s_config.statusLedEnabled = p.getBool("sled", true);
    p.end();

    // Oporavak od neispravnih vrednosti (specifikacija §17)
    if (!posixTimezone(s_config.timezone)) {
        strlcpy(s_config.timezone, "Europe/Belgrade", sizeof(s_config.timezone));
    }
    if (s_config.ntpIntervalMin < 15 || s_config.ntpIntervalMin > 1440) {
        s_config.ntpIntervalMin = 60;
    }
    if (s_config.oledBrightness > 100) {
        s_config.oledBrightness = 80;
    }
    LOG_I(TAG, "Konfiguracija učitana (Wi-Fi mreža: %s)", s_config.wifiSsid[0] ? s_config.wifiSsid : "nije podešena");
}

const DeviceConfig &get() { return s_config; }

DeviceConfig &edit() { return s_config; }

bool save() {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) {
        LOG_E(TAG, "Upis konfiguracije nije uspeo");
        return false;
    }
    p.putUInt("ver", CONFIG_VERSION);
    p.putString("ssid", s_config.wifiSsid);
    p.putString("wpass", s_config.wifiPass);
    p.putBool("static", s_config.staticIp);
    p.putUInt("ip", s_config.ip);
    p.putUInt("gw", s_config.gateway);
    p.putUInt("mask", s_config.subnet);
    p.putUInt("dns", s_config.dns);
    p.putString("host", s_config.hostname);
    p.putString("admin", s_config.adminPass);
    p.putString("appass", s_config.apPass);
    p.putBool("ntp", s_config.ntpEnabled);
    p.putString("ntp1", s_config.ntpServer1);
    p.putString("ntp2", s_config.ntpServer2);
    p.putString("ntp3", s_config.ntpServer3);
    p.putUShort("ntpint", s_config.ntpIntervalMin);
    p.putString("tz", s_config.timezone);
    p.putUChar("oledbr", s_config.oledBrightness);
    p.putBool("sled", s_config.statusLedEnabled);
    p.end();
    LOG_I(TAG, "Konfiguracija sačuvana");
    return true;
}

void resetToDefaults() {
    s_config = DeviceConfig();
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.clear();
        p.end();
    }
    LOG_W(TAG, "Konfiguracija vraćena na podrazumevane vrednosti");
}

const char *apPassword() { return s_config.apPass[0] ? s_config.apPass : s_defaultApPass; }

const char *adminPassword() { return s_config.adminPass[0] ? s_config.adminPass : apPassword(); }

bool adminPasswordSet() { return s_config.adminPass[0] != '\0'; }

const char *posixTimezone(const char *name) {
    for (const TimezoneDef &tz : TIMEZONES) {
        if (strcmp(tz.name, name) == 0) return tz.posix;
    }
    return nullptr;
}

size_t timezoneCount() { return sizeof(TIMEZONES) / sizeof(TIMEZONES[0]); }

const char *timezoneName(size_t index) { return index < timezoneCount() ? TIMEZONES[index].name : nullptr; }

}  // namespace config_manager
