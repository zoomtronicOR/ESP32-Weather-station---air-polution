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

    // Prikaz
    uint8_t oledBrightness = 80;  // 0–100 %; 0 = ekran isključen
    bool statusLedEnabled = true; // plava LED na ploči (kod treptanja, specifikacija §16.1)
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
