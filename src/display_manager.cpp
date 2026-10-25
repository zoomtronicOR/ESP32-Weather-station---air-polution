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

enum class Screen : uint8_t { Splash, Connection, Rotation };

enum Page : uint8_t { PAGE_BASIC, PAGE_AIR, PAGE_LIGHT, PAGE_EAQI, PAGE_STATUS, PAGE_COUNT };

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

static uint8_t s_shift = 0;
static uint32_t s_lastShiftMs = 0;

static ConnectionInfo s_conn;

// Tri reda teksta (font t0_11b, 21 znak u redu)
static constexpr uint8_t LINE_Y[3] = {9, 20, 31};

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
    // Podebljan font visine 11 px (tri reda staju u 32 px); sadrži č ć š ž đ, °, µ, ³
    s_oled.setFont(u8g2_font_t0_11b_te);
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

static void line(uint8_t row, const char *text) {
    s_oled.setCursor(s_shift & 1, LINE_Y[row] - ((s_shift >> 1) & 1));
    s_oled.print(text);
}

// Vrednost metrike kao tekst: broj sa jedinicom, "zagrevanje" ili "--"
static void formatMetric(char *buf, size_t len, MetricId id, uint32_t now, const char *unitOverride = nullptr,
                         float scale = 1.0f, int decimalsOverride = -1) {
    const MetricDef &d = metrics::def(id);
    const Reading &r = metrics::get(id);
    if (metrics::isFresh(id, now)) {
        int dec = decimalsOverride >= 0 ? decimalsOverride : d.decimals;
        const char *unit = unitOverride ? unitOverride : d.unit;
        snprintf(buf, len, "%.*f%s%s", dec, r.value * scale, unit[0] ? " " : "", unit);
    } else if (r.error == ErrorCode::Warmup) {
        snprintf(buf, len, "zagrevanje");
    } else {
        snprintf(buf, len, "--");
    }
}

// Red "naziv  vrednost"; naziv se dopunjava do 9 znakova (UTF-8 slova zauzimaju 2 bajta, a jedno mesto)
static void row(uint8_t r, const char *label, const char *value) {
    char buf[48];
    size_t pos = strlcpy(buf, label, sizeof(buf));
    size_t chars = 0;
    for (const char *p = label; *p; p++) {
        if ((*p & 0xC0) != 0x80) chars++;
    }
    while (chars++ < 9 && pos < sizeof(buf) - 1) buf[pos++] = ' ';
    buf[pos] = '\0';
    strlcat(buf, value, sizeof(buf));
    line(r, buf);
}

static void drawSplash() {
    line(0, "Meteo stanica");
    char buf[32];
    snprintf(buf, sizeof(buf), "Firmware %s", FW_VERSION);
    line(1, buf);
    line(2, DEVICE_NAME);
}

static void drawConnection() {
    char buf[40];
    switch (s_conn.mode) {
        case NetMode::Off:
            line(0, "Wi-Fi: isključen");
            line(1, "Lokalni rad");
            snprintf(buf, sizeof(buf), "v%s %s", FW_VERSION, DEVICE_NAME);
            line(2, buf);
            break;
        case NetMode::Connecting:
            line(0, "Wi-Fi: povezivanje...");
            line(1, s_conn.ssid);
            break;
        case NetMode::Connected:
            snprintf(buf, sizeof(buf), "WiFi %s", s_conn.ssid);
            line(0, buf);
            snprintf(buf, sizeof(buf), "IP %s", s_conn.ip);
            line(1, buf);
            snprintf(buf, sizeof(buf), "%d dBm  MQTT %s", s_conn.rssi,
                     !s_conn.mqttEnabled ? "isklj." : (s_conn.mqttConnected ? "OK" : "--"));
            line(2, buf);
            break;
        case NetMode::Portal:
            snprintf(buf, sizeof(buf), "AP: %s", s_conn.apSsid);
            line(0, buf);
            snprintf(buf, sizeof(buf), "Lozinka: %s", s_conn.apPassword);
            line(1, buf);
            snprintf(buf, sizeof(buf), "IP: %s", s_conn.apIp);
            line(2, buf);
            break;
    }
}

static void drawPage(uint8_t page, uint32_t now) {
    char v[24];
    switch (page) {
        case PAGE_BASIC:
            formatMetric(v, sizeof(v), MetricId::Temperature, now);
            row(0, "Temp.", v);
            formatMetric(v, sizeof(v), MetricId::Humidity, now);
            row(1, "Vlažnost", v);
            formatMetric(v, sizeof(v), MetricId::Pressure, now);
            row(2, "Pritisak", v);
            break;

        case PAGE_AIR:
            formatMetric(v, sizeof(v), MetricId::Pm2_5, now);
            row(0, "PM2.5", v);
            formatMetric(v, sizeof(v), MetricId::Pm10, now);
            row(1, "PM10", v);
            formatMetric(v, sizeof(v), MetricId::GasResistance, now, "kOhm", 0.001f, 1);
            row(2, "Gas", v);
            break;

        case PAGE_LIGHT: {
            formatMetric(v, sizeof(v), MetricId::UvIndex, now);
            row(0, "UV indeks", v);
            formatMetric(v, sizeof(v), MetricId::Illuminance, now);
            row(1, "Svetlo", v);
            if (metrics::isFresh(MetricId::Pitch, now)) {
                snprintf(v, sizeof(v), "%.0f° / %.0f°", metrics::get(MetricId::Pitch).value,
                         metrics::get(MetricId::Roll).value);
            } else {
                snprintf(v, sizeof(v), "--");
            }
            row(2, "Nagib", v);
            break;
        }

        case PAGE_EAQI: {
            line(0, "Kvalitet vazduha:");
            uint8_t lvl = air_quality::level();
            char buf[32];
            if (lvl == 0) {
                const Reading &pm = metrics::get(MetricId::Pm2_5);
                line(1, pm.error == ErrorCode::Warmup ? "zagrevanje SPS30" : "nepoznat");
            } else {
                snprintf(buf, sizeof(buf), "%s (%u/6)", air_quality::levelName(lvl), lvl);
                line(1, buf);
                snprintf(buf, sizeof(buf), "%s %.1f µg/m³", air_quality::dominant() == MetricId::Pm2_5 ? "PM2.5" : "PM10",
                         air_quality::dominantAverage());
                line(2, buf);
            }
            break;
        }

        case PAGE_STATUS: {
            char buf[32];
            const char *wifi = "isključen";
            if (s_conn.mode == NetMode::Connected) wifi = s_conn.ip;
            else if (s_conn.mode == NetMode::Connecting) wifi = "povezivanje";
            else if (s_conn.mode == NetMode::Portal) wifi = "portal";
            snprintf(buf, sizeof(buf), "WiFi %s", wifi);
            line(0, buf);
            snprintf(buf, sizeof(buf), "MQTT %s", !s_conn.mqttEnabled ? "isključen" : (s_conn.mqttConnected ? "OK" : "--"));
            line(1, buf);
            sensor_manager::Health h = sensor_manager::health();
            // Vreme se prikazuje posle NTP sinhronizacije (Faza 5); do tada --:--
            snprintf(buf, sizeof(buf), "Senzori %u/%u   --:--", h.ok, h.total);
            line(2, buf);
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
    s_connectionUntilMs = now + OLED_CONN_REPEAT_MS;
    s_lastRefreshMs = 0;  // odmah osvežiti
}

static void advanceScreen(uint32_t now) {
    switch (s_screen) {
        case Screen::Splash:
            if (now - s_screenStartMs >= OLED_SPLASH_MS) {
                s_screen = Screen::Connection;
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

    advanceScreen(now);

    if (now - s_lastShiftMs >= OLED_SHIFT_MS) {
        s_lastShiftMs = now;
        s_shift = (s_shift + 1) & 3;
    }

    if (s_lastRefreshMs != 0 && now - s_lastRefreshMs < OLED_REFRESH_MS) {
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
        case Screen::Connection: drawConnection(); break;
        case Screen::Rotation:
            if (s_conn.mode == NetMode::Portal && s_portalTurn) drawConnection();
            else drawPage(s_page, now);
            break;
    }
    s_oled.sendBuffer();
}

}  // namespace display_manager
