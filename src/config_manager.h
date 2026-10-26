#pragma once

#include <Arduino.h>

// Trajna konfiguracija uređaja u NVS (specifikacija §7, §12.2, §14)
// Lozinke se nikada ne ispisuju u log niti vraćaju kroz API.

struct DeviceConfig {
    // Wi-Fi
    char wifiSsid[33] = "";
    char wifiPass[65] = "";
    bool staticIp = false;
    uint32_t ip = 0, gateway = 0, subnet = 0, dns = 0;
    char hostname[32] = "meteo-esp32";

    // Pristup
    char adminPass[65] = "";  // prazno = koristi se lozinka pristupne tačke
    char apPass[65] = "";     // prazno = podrazumevana lozinka izvedena iz MAC adrese

    // Vreme (specifikacija §12.2)
    bool ntpEnabled = true;
    char ntpServer1[64] = "rs.pool.ntp.org";
    char ntpServer2[64] = "europe.pool.ntp.org";
    char ntpServer3[64] = "pool.ntp.org";
    uint16_t ntpIntervalMin = 60;
    char timezone[40] = "Europe/Belgrade";

    // MQTT (specifikacija §8)
    bool mqttEnabled = false;
    char mqttHost[64] = "";
    uint16_t mqttPort = 1883;
    char mqttUser[33] = "";
    char mqttPass[65] = "";
    char mqttClientId[33] = "esp32_r201";
    char mqttBaseTopic[64] = "meteo/esp32_r201";
    uint16_t mqttIntervalS = 10;      // 5–300 s
    uint8_t mqttQos = 0;              // 0 ili 1
    bool mqttDiscovery = true;        // Home Assistant MQTT Discovery
    char mqttDiscoveryPrefix[32] = "homeassistant";

    // Prikaz
    uint8_t oledBrightness = 80;  // 0–100 %; 0 = ekran isključen
    bool statusLedEnabled = true; // plava LED na ploči (kod treptanja, specifikacija §16.1)

    // WS2812B LED indikator kvaliteta vazduha (specifikacija §10A)
    bool ledEnabled = false;      // isključen dok dioda nije povezana
    uint8_t ledCount = 1;         // 1–16
    uint8_t ledBrightness = 30;   // 0–100 %
    uint8_t ledMode = 0;          // 0 = sve diode istom bojom, 1 = bar-prikaz
    uint8_t ledSource = 0;        // 0 = lošiji od PM2.5/PM10, 1 = PM2.5, 2 = PM10
    bool ledAlarmPulse = true;    // pulsiranje od nivoa 4 (Loš)
    bool ledAutoBrightness = false;  // prema osvetljenosti (LTR-390)
    bool ledNightEnabled = true;
    uint8_t ledNightStart = 22;   // sat (lokalno vreme)
    uint8_t ledNightEnd = 7;
    uint8_t ledNightBrightness = 5;  // %; 0 = isključena noću

    // Dijagnostika
    uint8_t logLevel = 2;         // 0 = ERROR ... 3 = DEBUG (specifikacija §16)
};

namespace config_manager {

void begin();

const DeviceConfig &get();
// Izmena: menja se kopija, pa se poziva save()
DeviceConfig &edit();
bool save();
void resetToDefaults();

// Podrazumevana lozinka pristupne tačke: jedinstvena po uređaju, prikazuje se na OLED-u u portalu
const char *apPassword();
// Lozinka za administratorske akcije u web interfejsu
const char *adminPassword();
bool adminPasswordSet();

// POSIX TZ string za podržane vremenske zone; nullptr ako zona nije podržana
const char *posixTimezone(const char *name);
// Lista podržanih zona (za web interfejs)
size_t timezoneCount();
const char *timezoneName(size_t index);

}  // namespace config_manager
