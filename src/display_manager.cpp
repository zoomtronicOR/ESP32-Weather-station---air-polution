#include "display_manager.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "air_quality.h"
#include "config.h"
#include "i2c_bus.h"
#include "log.h"
#include "metrics.h"
#include "sensor_manager.h"

namespace display_manager {

static const char *TAG = "oled";

// Ekran deli glavnu I2C magistralu (Wire) koju je već pokrenuo i2c_bus
static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C s_oled(U8G2_R0, U8X8_PIN_NONE);

// Dva reda po ekranu. Osnovni font: podebljan 8×15 px (16 znakova u redu);
// red koji ne staje crta se užim fontom 7×13 px (18 znakova). Oba sadrže č ć š ž đ, °, µ, ³.
static const uint8_t *const FONT_BIG = u8g2_font_t0_16b_te;
static const uint8_t *const FONT_NARROW = u8g2_font_t0_14b_te;
static constexpr int16_t ROW_BASELINE[2] = {12, 28};
static constexpr int16_t WIDTH = 128;
static constexpr int16_t LABEL_GAP = 6;

enum class Screen : uint8_t { Splash, Connection, Rotation };

enum Page : uint8_t {
    PAGE_TEMP_HUM,
    PAGE_PRESSURE_GAS,
    PAGE_PM_SMALL,
    PAGE_PM_LARGE,
    PAGE_EAQI,
    PAGE_UV_LIGHT,
    PAGE_TILT,
    PAGE_NETWORK,
    PAGE_SYSTEM,
    PAGE_COUNT
};

static bool s_available = false;
static uint8_t s_address = 0x3C;
static uint32_t s_lastRetryMs = 0;
static uint32_t s_lastRefreshMs = 0;

static Screen s_screen = Screen::Splash;
static uint32_t s_screenStartMs = 0;
static uint32_t s_connectionUntilMs = 0;
static uint8_t s_page = 0;
static uint32_t s_pageStartMs = 0;
static bool s_portalTurn = true;

// Zaštita od burn-in: sadržaj se naizmenično pomera za 1 piksel horizontalno
static uint8_t s_shiftX = 0;
static uint32_t s_lastShiftMs = 0;

static ConnectionInfo s_conn;

static bool detect() {
    for (uint8_t addr : {0x3C, 0x3D}) {
        if (i2c_bus::probe(Wire, addr)) {
            s_address = addr;
            return true;
        }
    }
    return false;
}

static void applyMaxBrightness() {
    // Ugrađeni ekran je slabo osvetljen -- maksimalni kontrast, pre-charge i VCOMH (specifikacija §10.2)
    s_oled.setContrast(255);
    s_oled.sendF("ca", 0xD9, 0xF1);
    s_oled.sendF("ca", 0xDB, 0x40);
}

static bool init() {
    if (!detect()) {
        return false;
    }
    s_oled.setI2CAddress(s_address << 1);
    s_oled.setBusClock(I2C_FREQUENCY_HZ);  // U8g2 bi inače prebacio magistralu na 400 kHz
    if (!s_oled.begin()) {
        return false;
    }
    s_oled.enableUTF8Print();
    applyMaxBrightness();
    return true;
}

void begin(uint32_t now) {
    s_available = init();
    s_lastRetryMs = now;
    s_screen = Screen::Splash;
    s_screenStartMs = now;
    if (s_available) {
        LOG_I(TAG, "OLED SSD1306 128×32 na adresi 0x%02X", s_address);
    } else {
        LOG_W(TAG, "OLED nije pronađen -- merenje se nastavlja bez ekrana");
    }
}

bool available() { return s_available; }

// Jedan red: naziv levo, vrednost uz desnu ivicu. Bez naziva vrednost se crta od leve ivice.
static void drawRow(uint8_t row, const char *label, const char *value) {
    const uint8_t *fonts[2] = {FONT_BIG, FONT_NARROW};
    for (const uint8_t *font : fonts) {
        s_oled.setFont(font);
        int16_t labelW = label[0] ? s_oled.getUTF8Width(label) : 0;
        int16_t valueW = s_oled.getUTF8Width(value);
        int16_t needed = labelW + (label[0] ? LABEL_GAP : 0) + valueW + s_shiftX;
        if (needed <= WIDTH || font == FONT_NARROW) {
            int16_t y = ROW_BASELINE[row];
            if (label[0]) {
                s_oled.drawUTF8(s_shiftX, y, label);
                s_oled.drawUTF8(WIDTH - valueW - (1 - s_shiftX), y, value);
            } else {
                s_oled.drawUTF8(s_shiftX, y, value);
            }
            return;
        }
    }
}

static void drawRows(const char *label1, const char *value1, const char *label2, const char *value2) {
    drawRow(0, label1, value1);
    drawRow(1, label2, value2);
}

// Vrednost metrike kao tekst: broj sa jedinicom, "zagrevanje" ili "--"
static void formatMetric(char *buf, size_t len, MetricId id, uint32_t now, const char *unitOverride = nullptr,
                         float scale = 1.0f, int decimalsOverride = -1) {
    const MetricDef &d = metrics::def(id);
    const Reading &r = metrics::get(id);
    if (metrics::isFresh(id, now)) {
        float v = r.value * scale;
        int dec = decimalsOverride >= 0 ? decimalsOverride : d.decimals;
        if (fabsf(v) >= 1000.0f) dec = 0;  // velike vrednosti bez decimala, da stanu u red
        const char *unit = unitOverride ? unitOverride : d.unit;
        snprintf(buf, len, "%.*f%s%s", dec, v, unit[0] ? " " : "", unit);
    } else if (r.error == ErrorCode::Warmup) {
        snprintf(buf, len, "zagrevanje");
    } else {
        snprintf(buf, len, "--");
    }
}

static const char *mqttText() {
    if (!s_conn.mqttEnabled) return "isključen";
    return s_conn.mqttConnected ? "povezan" : "nije povezan";
}

static void drawSplash() {
    char buf[24];
    snprintf(buf, sizeof(buf), "Firmware %s", FW_VERSION);
    drawRows("", "Meteo stanica", "", buf);
}

static void drawConnection(uint32_t now) {
    char buf[40];
    // Kada ima više od dva podatka, smenjuju se dva para na 3 s
    bool second = ((now - s_screenStartMs) / 3000) % 2 == 1;
    switch (s_conn.mode) {
        case NetMode::Off:
            drawRows("", "Wi-Fi isključen", "", "Lokalni rad");
            break;
        case NetMode::Connecting:
            drawRows("", "Wi-Fi povezivanje", "", s_conn.ssid);
            break;
        case NetMode::Connected:
            if (!second) {
                drawRows("", s_conn.ssid, "", s_conn.ip);
            } else {
                snprintf(buf, sizeof(buf), "%d dBm", s_conn.rssi);
                drawRows("Signal", buf, "MQTT", mqttText());
            }
            break;
        case NetMode::Portal:
            if (!second) {
                drawRows("", s_conn.apSsid, "", s_conn.apPassword);
            } else {
                drawRows("", "Podešavanje:", "", s_conn.apIp);
            }
            break;
    }
}

static void drawPage(uint8_t page, uint32_t now) {
    char v1[32], v2[32];
    switch (page) {
        case PAGE_TEMP_HUM:
            formatMetric(v1, sizeof(v1), MetricId::Temperature, now);
            formatMetric(v2, sizeof(v2), MetricId::Humidity, now);
            drawRows("Temp.", v1, "Vlažnost", v2);
            break;

        case PAGE_PRESSURE_GAS:
            formatMetric(v1, sizeof(v1), MetricId::Pressure, now);
            formatMetric(v2, sizeof(v2), MetricId::GasResistance, now, "kOhm", 0.001f, 1);
            drawRows("Pritisak", v1, "Gas", v2);
            break;

        case PAGE_PM_SMALL:
            formatMetric(v1, sizeof(v1), MetricId::Pm1_0, now);
            formatMetric(v2, sizeof(v2), MetricId::Pm2_5, now);
            drawRows("PM1.0", v1, "PM2.5", v2);
            break;

        case PAGE_PM_LARGE:
            formatMetric(v1, sizeof(v1), MetricId::Pm4, now);
            formatMetric(v2, sizeof(v2), MetricId::Pm10, now);
            drawRows("PM4", v1, "PM10", v2);
            break;

        case PAGE_EAQI: {
            uint8_t lvl = air_quality::level();
            if (lvl == 0) {
                const Reading &pm = metrics::get(MetricId::Pm2_5);
                drawRows("", "Kvalitet vazduha", "", pm.error == ErrorCode::Warmup ? "zagrevanje" : "nepoznat");
            } else {
                snprintf(v2, sizeof(v2), "%s %u/6", air_quality::levelName(lvl), lvl);
                drawRows("", "Kvalitet vazduha", "", v2);
            }
            break;
        }

        case PAGE_UV_LIGHT:
            formatMetric(v1, sizeof(v1), MetricId::UvIndex, now);
            formatMetric(v2, sizeof(v2), MetricId::Illuminance, now);
            drawRows("UV indeks", v1, "Svetlo", v2);
            break;

        case PAGE_TILT:
            if (metrics::isFresh(MetricId::Pitch, now)) {
                snprintf(v1, sizeof(v1), "%.0f°/%.0f°", metrics::get(MetricId::Pitch).value,
                         metrics::get(MetricId::Roll).value);
            } else {
                snprintf(v1, sizeof(v1), "--");
            }
            if (metrics::isFresh(MetricId::Motion, now)) {
                snprintf(v2, sizeof(v2), "%s", metrics::get(MetricId::Motion).value > 0.5f ? "DA" : "ne");
            } else {
                snprintf(v2, sizeof(v2), "--");
            }
            drawRows("Nagib", v1, "Pomeranje", v2);
            break;

        case PAGE_NETWORK: {
            const char *wifi = "isključen";
            if (s_conn.mode == NetMode::Connected) wifi = s_conn.ip;
            else if (s_conn.mode == NetMode::Connecting) wifi = "povezivanje";
            else if (s_conn.mode == NetMode::Portal) wifi = "portal";
            drawRows("WiFi", wifi, "MQTT", mqttText());
            break;
        }

        case PAGE_SYSTEM: {
            sensor_manager::Health h = sensor_manager::health();
            snprintf(v1, sizeof(v1), "%u/%u", h.ok, h.total);
            // Vreme se prikazuje posle NTP sinhronizacije (Faza 5); do tada --:--
            drawRows("Senzori", v1, "Vreme", "--:--");
            break;
        }
    }
}

void setConnectionInfo(const ConnectionInfo &info, uint32_t now) {
    bool changed = info.mode != s_conn.mode || strcmp(info.ip, s_conn.ip) != 0;
    s_conn = info;
    if (changed && s_screen == Screen::Rotation) {
        showConnection(now);
    }
}

void showConnection(uint32_t now) {
    if (s_screen == Screen::Splash) return;
    s_screen = Screen::Connection;
    s_screenStartMs = now;
    s_connectionUntilMs = now + OLED_CONN_REPEAT_MS;
    s_lastRefreshMs = 0;  // odmah osvežiti
}

static void advanceScreen(uint32_t now) {
    switch (s_screen) {
        case Screen::Splash:
            if (now - s_screenStartMs >= OLED_SPLASH_MS) {
                s_screen = Screen::Connection;
                s_screenStartMs = now;
                s_connectionUntilMs = now + OLED_BOOT_CONN_MS;
            }
            break;
        case Screen::Connection:
            if ((int32_t)(now - s_connectionUntilMs) >= 0) {
                s_screen = Screen::Rotation;
                s_page = 0;
                s_pageStartMs = now;
            }
            break;
        case Screen::Rotation:
            // Dok je portal aktivan, podaci za konekciju se smenjuju sa stranicama i prikazuju duže
            if (s_conn.mode == NetMode::Portal) {
                uint32_t slot = s_portalTurn ? 2 * OLED_PAGE_MS : OLED_PAGE_MS;
                if (now - s_pageStartMs >= slot) {
                    s_pageStartMs = now;
                    s_portalTurn = !s_portalTurn;
                    if (!s_portalTurn) s_page = (s_page + 1) % PAGE_COUNT;
                }
            } else if (now - s_pageStartMs >= OLED_PAGE_MS) {
                s_pageStartMs = now;
                s_page = (s_page + 1) % PAGE_COUNT;
            }
            break;
    }
}

void update(uint32_t now) {
    if (!s_available) {
        if (now - s_lastRetryMs >= OLED_RETRY_MS) {
            s_lastRetryMs = now;
            s_available = init();
            if (s_available) LOG_I(TAG, "OLED ponovo dostupan");
        }
        return;
    }

    uint8_t prevPage = s_page;
    Screen prevScreen = s_screen;
    advanceScreen(now);
    bool changed = s_page != prevPage || s_screen != prevScreen;

    if (now - s_lastShiftMs >= OLED_SHIFT_MS) {
        s_lastShiftMs = now;
        s_shiftX ^= 1;
    }

    if (!changed && s_lastRefreshMs != 0 && now - s_lastRefreshMs < OLED_REFRESH_MS) {
        return;
    }
    s_lastRefreshMs = now;

    // Greška ekrana ne sme da zaustavi merenje: proverava se odziv pre slanja
    if (!i2c_bus::probe(Wire, s_address)) {
        s_available = false;
        s_lastRetryMs = now;
        LOG_W(TAG, "OLED ne odgovara -- novi pokušaj za %lu s", (unsigned long)(OLED_RETRY_MS / 1000));
        return;
    }

    s_oled.clearBuffer();
    switch (s_screen) {
        case Screen::Splash:     drawSplash(); break;
        case Screen::Connection: drawConnection(now); break;
        case Screen::Rotation:
            if (s_conn.mode == NetMode::Portal && s_portalTurn) drawConnection(now);
            else drawPage(s_page, now);
            break;
    }
    s_oled.sendBuffer();
}

}  // namespace display_manager
