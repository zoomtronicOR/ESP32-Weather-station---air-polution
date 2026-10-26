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
    s_config.logLevel = min<uint8_t>(3, p.getUChar("loglvl", 2));
    s_config.ledEnabled = p.getBool("led", false);
    s_config.ledCount = constrain(p.getUChar("ledn", 1), 1, 16);
    s_config.ledBrightness = min<uint8_t>(100, p.getUChar("ledbr", 30));
    s_config.ledMode = min<uint8_t>(1, p.getUChar("ledmode", 0));
    s_config.ledSource = min<uint8_t>(2, p.getUChar("ledsrc", 0));
    s_config.ledAlarmPulse = p.getBool("ledpulse", true);
    s_config.ledAutoBrightness = p.getBool("ledauto", false);
    s_config.ledNightEnabled = p.getBool("lednight", true);
    s_config.ledNightStart = min<uint8_t>(23, p.getUChar("lednfrom", 22));
    s_config.ledNightEnd = min<uint8_t>(23, p.getUChar("lednto", 7));
    s_config.ledNightBrightness = min<uint8_t>(100, p.getUChar("lednbr", 5));
    s_config.mqttEnabled = p.getBool("mqtt", false);
    loadString(p, "mhost", s_config.mqttHost, sizeof(s_config.mqttHost));
    s_config.mqttPort = p.getUShort("mport", 1883);
    loadString(p, "muser", s_config.mqttUser, sizeof(s_config.mqttUser));
    loadString(p, "mpass", s_config.mqttPass, sizeof(s_config.mqttPass));
    loadString(p, "mcid", s_config.mqttClientId, sizeof(s_config.mqttClientId));
    loadString(p, "mbase", s_config.mqttBaseTopic, sizeof(s_config.mqttBaseTopic));
    s_config.mqttIntervalS = p.getUShort("mint", 10);
    s_config.mqttQos = p.getUChar("mqos", 0);
    s_config.mqttDiscovery = p.getBool("mdisc", true);
    loadString(p, "mdpre", s_config.mqttDiscoveryPrefix, sizeof(s_config.mqttDiscoveryPrefix));
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
    if (s_config.mqttIntervalS < 5 || s_config.mqttIntervalS > 300) {
        s_config.mqttIntervalS = 10;
    }
    if (s_config.mqttQos > 1) {
        s_config.mqttQos = 0;
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
    p.putUChar("loglvl", s_config.logLevel);
    p.putBool("led", s_config.ledEnabled);
    p.putUChar("ledn", s_config.ledCount);
    p.putUChar("ledbr", s_config.ledBrightness);
    p.putUChar("ledmode", s_config.ledMode);
    p.putUChar("ledsrc", s_config.ledSource);
    p.putBool("ledpulse", s_config.ledAlarmPulse);
    p.putBool("ledauto", s_config.ledAutoBrightness);
    p.putBool("lednight", s_config.ledNightEnabled);
    p.putUChar("lednfrom", s_config.ledNightStart);
    p.putUChar("lednto", s_config.ledNightEnd);
    p.putUChar("lednbr", s_config.ledNightBrightness);
    p.putBool("mqtt", s_config.mqttEnabled);
    p.putString("mhost", s_config.mqttHost);
    p.putUShort("mport", s_config.mqttPort);
    p.putString("muser", s_config.mqttUser);
    p.putString("mpass", s_config.mqttPass);
    p.putString("mcid", s_config.mqttClientId);
    p.putString("mbase", s_config.mqttBaseTopic);
    p.putUShort("mint", s_config.mqttIntervalS);
    p.putUChar("mqos", s_config.mqttQos);
    p.putBool("mdisc", s_config.mqttDiscovery);
    p.putString("mdpre", s_config.mqttDiscoveryPrefix);
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
