#pragma once

#include <Arduino.h>

// OLED 128×32 SSD1306: podaci za konekciju pri pokretanju, zatim rotacija stranica (specifikacija §10)

enum class NetMode : uint8_t {
    Off,         // Wi-Fi nije aktivan
    Connecting,  // povezivanje u toku
    Connected,   // povezan na mrežu
    Portal,      // aktivan konfiguracioni portal
};

// Podaci za konekciju; popunjava ih wifi_manager (Faza 5)
struct ConnectionInfo {
    NetMode mode = NetMode::Off;
    char ssid[33] = "";
    char ip[16] = "";
    int rssi = 0;
    char apSsid[33] = "";
    char apPassword[65] = "";  // prikazuje se samo dok je portal aktivan
    char apIp[16] = "";
    bool mqttEnabled = false;
    bool mqttConnected = false;
};

namespace display_manager {

void begin(uint32_t now);
void update(uint32_t now);

void setConnectionInfo(const ConnectionInfo &info, uint32_t now);
// Ponovo prikazuje podatke za konekciju (npr. na pritisak tastera BOOT)
void showConnection(uint32_t now);

bool available();

// Primenjuje osvetljenje iz konfiguracije (0–100 %; 0 = ekran isključen)
void applyBrightness();

}  // namespace display_manager
