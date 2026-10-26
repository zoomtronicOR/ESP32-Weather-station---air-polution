#include "display_manager.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "air_quality.h"
#include "config.h"
#include "config_manager.h"
#include "i2c_bus.h"
#include "log.h"
#include "metrics.h"
#include "ota_manager.h"
#include "sensor_manager.h"
#include "time_manager.h"

namespace display_manager {

static const char *TAG = "oled";

// OLED 0,96" 128×64 SSD1306, dvobojni: gornjih 16 redova piksela žuto, ostatak plavo.
// Ekran deli glavnu I2C magistralu (Wire) koju je već pokrenuo i2c_bus.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, U8X8_PIN_NONE);

// Žuti pojas (0–15): stalno vreme i datum. Plavi deo (16–63): tri reda stranice koja se smenjuje.
// Osnovni font: podebljan 8×15 px (16 znakova); red koji ne staje crta se užim fontom 7×13 px (18 znakova).
// Oba fonta sadrže č ć š ž đ, °, µ, ³.
static const uint8_t *const FONT_BIG = u8g2_font_t0_16b_te;
static const uint8_t *const FONT_NARROW = u8g2_font_t0_14b_te;
static constexpr int16_t HEADER_BASELINE = 12;
static constexpr int16_t ROW_BASELINE[3] = {28, 44, 60};
static constexpr int16_t WIDTH = 128;
static constexpr int16_t LABEL_GAP = 6;
static constexpr uint8_t ROWS = 3;

enum class Screen : uint8_t { Splash, Connection, Rotation };

enum Page : uint8_t { PAGE_CLIMATE, PAGE_PM, PAGE_AIR, PAGE_LIGHT, PAGE_NETWORK, PAGE_SYSTEM, PAGE_COUNT };

// Sadržaj plavog dela: do tri reda "naziv  vrednost" (prazan naziv = vrednost od leve ivice)
struct Content {
    char label[ROWS][20];
    char value[ROWS][40];
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
    applyBrightness();
    return true;
}

void applyBrightness() {
    uint8_t pct = config_manager::get().oledBrightness;
    if (pct == 0) {
        s_oled.setPowerSave(1);
        return;
    }
    s_oled.setPowerSave(0);
    // Kontrast sam menja osvetljenje u uskom opsegu; pre-charge i VCOMH proširuju opseg
    s_oled.setContrast((uint8_t)(1 + (uint16_t)pct * 254 / 100));
    s_oled.sendF("ca", 0xD9, pct >= 50 ? 0xF1 : 0x22);  // pre-charge period
    s_oled.sendF("ca", 0xDB, pct >= 50 ? 0x40 : 0x20);  // VCOMH nivo
}

void begin(uint32_t now) {
    s_available = init();
    s_lastRetryMs = now;
    s_screen = Screen::Splash;
    s_screenStartMs = now;
    if (s_available) {
        LOG_I(TAG, "OLED SSD1306 128×64 na adresi 0x%02X", s_address);
    } else {
        LOG_W(TAG, "OLED nije pronađen -- merenje se nastavlja bez ekrana");
    }
}

bool available() { return s_available; }

// ---------------------------------------------------------------------------
// Crtanje
// ---------------------------------------------------------------------------

// Jedan red: naziv levo, vrednost uz desnu ivicu. Bez naziva vrednost se crta od leve ivice.
static void drawLine(int16_t baseline, const char *label, const char *value) {
    const uint8_t *fonts[2] = {FONT_BIG, FONT_NARROW};
    for (const uint8_t *font : fonts) {
        s_oled.setFont(font);
        int16_t labelW = label[0] ? s_oled.getUTF8Width(label) : 0;
        int16_t valueW = s_oled.getUTF8Width(value);
        int16_t needed = labelW + (label[0] ? LABEL_GAP : 0) + valueW + s_shiftX;
        if (needed <= WIDTH || font == FONT_NARROW) {
            if (label[0]) {
                s_oled.drawUTF8(s_shiftX, baseline, label);
                s_oled.drawUTF8(WIDTH - valueW - (1 - s_shiftX), baseline, value);
            } else {
                s_oled.drawUTF8(s_shiftX, baseline, value);
            }
            return;
        }
    }
}

// Žuti pojas: vreme levo, dan i datum desno -- ne menja se sa stranicama
static void drawHeader() {
    static const char *const DAYS[7] = {"ned", "pon", "uto", "sre", "čet", "pet", "sub"};
    char clock[8], right[24];
    if (time_manager::isValid()) {
        time_t t = time_manager::nowUtc();
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(clock, sizeof(clock), "%H:%M", &tmv);
        snprintf(right, sizeof(right), "%s %02d.%02d.", DAYS[tmv.tm_wday], tmv.tm_mday, tmv.tm_mon + 1);
    } else {
        strlcpy(clock, "--:--", sizeof(clock));
        strlcpy(right, time_manager::status() == time_manager::TimeStatus::Syncing ? "čeka NTP" : "nema vreme",
                sizeof(right));
    }
    drawLine(HEADER_BASELINE, clock, right);
}

static void drawContent(const Content &c) {
    for (uint8_t r = 0; r < ROWS; r++) {
        if (c.label[r][0] || c.value[r][0]) {
            drawLine(ROW_BASELINE[r], c.label[r], c.value[r]);
        }
    }
}

static void setRow(Content &c, uint8_t row, const char *label, const char *value) {
    strlcpy(c.label[row], label, sizeof(c.label[row]));
    strlcpy(c.value[row], value, sizeof(c.value[row]));
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

static void metricRow(Content &c, uint8_t row, const char *label, MetricId id, uint32_t now,
                      const char *unitOverride = nullptr, float scale = 1.0f, int decimalsOverride = -1) {
    char v[40];
    formatMetric(v, sizeof(v), id, now, unitOverride, scale, decimalsOverride);
    setRow(c, row, label, v);
}

static const char *mqttText() {
    if (!s_conn.mqttEnabled) return "isključen";
    return s_conn.mqttConnected ? "povezan" : "nije povezan";
}

static void buildSplash(Content &c) {
    char buf[24];
    snprintf(buf, sizeof(buf), "Firmware %s", FW_VERSION);
    setRow(c, 0, "", "Meteo stanica");
    setRow(c, 1, "", buf);
    setRow(c, 2, "", DEVICE_NAME);
}

static void buildConnection(Content &c) {
    char buf[40];
    switch (s_conn.mode) {
        case NetMode::Off:
            setRow(c, 0, "", "Wi-Fi isključen");
            setRow(c, 1, "", "Lokalni rad");
            break;
        case NetMode::Connecting:
            setRow(c, 0, "", "Wi-Fi povezivanje");
            setRow(c, 1, "", s_conn.ssid);
            break;
        case NetMode::Connected:
            setRow(c, 0, "", s_conn.ssid);
            setRow(c, 1, "", s_conn.ip);
            snprintf(buf, sizeof(buf), "%d dBm", s_conn.rssi);
            setRow(c, 2, "Signal", buf);
            break;
        case NetMode::Portal:
            setRow(c, 0, "", s_conn.apSsid);
            setRow(c, 1, "", s_conn.apPassword);
            setRow(c, 2, "", s_conn.apIp);
            break;
    }
}

static void buildPage(Content &c, uint8_t page, uint32_t now) {
    char v[40];
    switch (page) {
        case PAGE_CLIMATE:
            metricRow(c, 0, "Temp.", MetricId::Temperature, now);
            metricRow(c, 1, "Vlažnost", MetricId::Humidity, now);
            metricRow(c, 2, "Pritisak", MetricId::Pressure, now);
            break;

        case PAGE_PM:
            metricRow(c, 0, "PM1.0", MetricId::Pm1_0, now);
            metricRow(c, 1, "PM2.5", MetricId::Pm2_5, now);
            metricRow(c, 2, "PM10", MetricId::Pm10, now);
            break;

        case PAGE_AIR: {
            uint8_t lvl = air_quality::level();
            if (lvl == 0) {
                const Reading &pm = metrics::get(MetricId::Pm2_5);
                setRow(c, 0, "Vazduh", pm.error == ErrorCode::Warmup ? "zagrevanje" : "nepoznat");
            } else {
                snprintf(v, sizeof(v), "%s %u/6", air_quality::levelName(lvl), lvl);
                setRow(c, 0, "Vazduh", v);
            }
            metricRow(c, 1, "PM4", MetricId::Pm4, now);
            metricRow(c, 2, "Gas", MetricId::GasResistance, now, "kOhm", 0.001f, 1);
            break;
        }

        case PAGE_LIGHT:
            metricRow(c, 0, "UV indeks", MetricId::UvIndex, now);
            metricRow(c, 1, "Svetlo", MetricId::Illuminance, now);
            if (metrics::isFresh(MetricId::Pitch, now)) {
                snprintf(v, sizeof(v), "%.0f°/%.0f°", metrics::get(MetricId::Pitch).value,
                         metrics::get(MetricId::Roll).value);
            } else {
                snprintf(v, sizeof(v), "--");
            }
            setRow(c, 2, "Nagib", v);
            break;

        case PAGE_NETWORK: {
            const char *wifi = "isključen";
            if (s_conn.mode == NetMode::Connected) wifi = s_conn.ip;
            else if (s_conn.mode == NetMode::Connecting) wifi = "povezivanje";
            else if (s_conn.mode == NetMode::Portal) wifi = "portal";
            setRow(c, 0, "WiFi", wifi);
            if (s_conn.mode == NetMode::Connected) snprintf(v, sizeof(v), "%d dBm", s_conn.rssi);
            else snprintf(v, sizeof(v), "--");
            setRow(c, 1, "Signal", v);
            setRow(c, 2, "MQTT", mqttText());
            break;
        }

        case PAGE_SYSTEM: {
            sensor_manager::Health h = sensor_manager::health();
            snprintf(v, sizeof(v), "%u/%u", h.ok, h.total);
            setRow(c, 0, "Senzori", v);
            if (metrics::isFresh(MetricId::Motion, now)) {
                setRow(c, 1, "Pomeranje", metrics::get(MetricId::Motion).value > 0.5f ? "DA" : "ne");
            } else {
                setRow(c, 1, "Pomeranje", "--");
            }
            uint32_t s = now / 1000;
            if (s >= 86400) snprintf(v, sizeof(v), "%lud %02luh", (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24));
            else snprintf(v, sizeof(v), "%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
            setRow(c, 2, "Rad", v);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Smena ekrana
// ---------------------------------------------------------------------------

void showOtaProgress(uint8_t percent, const char *what) {
    if (!s_available) return;
    char buf[24];
    s_oled.clearBuffer();
    drawLine(HEADER_BASELINE, "", "Ažuriranje");          // žuti pojas
    drawLine(ROW_BASELINE[0], "", what);
    snprintf(buf, sizeof(buf), "%u %%", percent);
    drawLine(ROW_BASELINE[1], "Napredak", buf);
    s_oled.drawFrame(0, 50, 128, 12);
    s_oled.drawBox(2, 52, (uint16_t)124 * percent / 100, 8);
    s_oled.sendBuffer();
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

    // Tokom ažuriranja iz web interfejsa prikazuje se napredak umesto stranica
    if (ota_manager::inProgress()) {
        if (now - s_lastRefreshMs >= OLED_REFRESH_MS) {
            s_lastRefreshMs = now;
            showOtaProgress(ota_manager::progress(), ota_manager::target());
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

    Content c = {};
    switch (s_screen) {
        case Screen::Splash:     buildSplash(c); break;
        case Screen::Connection: buildConnection(c); break;
        case Screen::Rotation:
            if (s_conn.mode == NetMode::Portal && s_portalTurn) buildConnection(c);
            else buildPage(c, s_page, now);
            break;
    }

    s_oled.clearBuffer();
    drawHeader();
    drawContent(c);
    s_oled.sendBuffer();
}

}  // namespace display_manager
