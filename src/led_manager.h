#pragma once

#include <Arduino.h>

// WS2812B LED indikator zagađenosti vazduha (specifikacija §10A)
// Boja po EAQI nivou, posebna stanja (zagrevanje, senzor nedostupan, portal, OTA),
// osvetljenje sa ograničenjem struje, noćni režim i opciono prilagođavanje svetlu.
// Signal se generiše RMT periferijom (ne "bit-bang"), da Wi-Fi prekidi ne remete tajming.

namespace led_manager {

enum class LedState : uint8_t {
    Off,          // isključen u podešavanjima ili noću
    BootTest,     // test boja pri pokretanju
    Level,        // boja EAQI nivoa
    Warmup,       // SPS30 se zagreva / još nema merenja
    SensorFault,  // PM merenje nevalidno ili SPS30 nedostupan
    Portal,       // konfiguracioni portal aktivan
    Ota,          // ažuriranje u toku
};

void begin();
void update(uint32_t now);
// Podešavanja su promenjena (broj dioda, osvetljenje, uključenost...)
void applyConfig();

// Za Home Assistant (light entitet): uključivanje i osvetljenje 0–255
void setFromHomeAssistant(bool on, int brightness255);

LedState state();
const char *stateKey(LedState s);
uint32_t currentColor();     // RGB 0xRRGGBB (pre primene osvetljenja)
uint8_t effectiveBrightness();  // % posle noćnog režima, auto-osvetljenja i ograničenja struje
uint8_t level();             // prikazani EAQI nivo (0 = nepoznat)
bool nightActive();

}  // namespace led_manager
