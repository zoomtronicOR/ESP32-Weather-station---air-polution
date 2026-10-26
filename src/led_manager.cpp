#include "led_manager.h"

#include <math.h>

#include "air_quality.h"
#include "config.h"
#include "config_manager.h"
#include "log.h"
#include "metrics.h"
#include "ota_manager.h"
#include "pins.h"
#include "time_manager.h"
#include "wifi_manager.h"

namespace led_manager {

static const char *TAG = "ws2812b";

// Boje EAQI nivoa, prilagođene vidljivosti na LED diodi (specifikacija §10A.1)
static const uint32_t LEVEL_COLORS[7] = {0x000000, 0x00C8FF, 0x00FF40, 0xFFD000, 0xFF6000, 0xFF0000, 0xB000FF};

// WS2812B tajming u koracima od 100 ns (RMT na 10 MHz): 0 = 0,4/0,85 µs, 1 = 0,8/0,45 µs
static constexpr uint32_t T0H = 4, T0L = 8, T1H = 8, T1L = 5;

static bool s_rmtReady = false;
static rmt_data_t s_symbols[LED_MAX_COUNT * 24];
static uint32_t s_frame[LED_MAX_COUNT];  // poslednje poslato (posle osvetljenja), za izbegavanje suvišnih upisa
static bool s_frameValid = false;

static LedState s_state = LedState::Off;
static uint32_t s_stateSinceMs = 0;
static uint32_t s_bootTestUntilMs = 0;
static uint32_t s_lastFrameMs = 0;
static uint8_t s_level = 0;
static uint8_t s_effectiveBrightness = 0;
static bool s_night = false;

// Prelaz između boja (fade)
static uint32_t s_fromColor = 0, s_toColor = 0, s_shownColor = 0;
static uint32_t s_fadeStartMs = 0;

// Izmene iz Home Assistant-a se čuvaju u NVS sa odlaganjem (klizač šalje mnogo poruka)
static uint32_t s_saveAtMs = 0;

// ---------------------------------------------------------------------------

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

static uint32_t scale(uint32_t c, float k) {
    if (k <= 0) return 0;
    if (k > 1) k = 1;
    return rgb((uint8_t)(((c >> 16) & 0xFF) * k + 0.5f), (uint8_t)(((c >> 8) & 0xFF) * k + 0.5f),
               (uint8_t)((c & 0xFF) * k + 0.5f));
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    auto ch = [&](int sh) { return (uint8_t)(((a >> sh) & 0xFF) + ((((int)((b >> sh) & 0xFF)) - (int)((a >> sh) & 0xFF)) * t)); };
    return rgb(ch(16), ch(8), ch(0));
}

static void ensureRmt() {
    if (s_rmtReady) return;
    s_rmtReady = rmtInit(PIN_WS2812B, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
    if (!s_rmtReady) LOG_E(TAG, "RMT nije inicijalizovan na GPIO%d", PIN_WS2812B);
}

static void show(const uint32_t *colors, uint8_t count) {
    if (!s_rmtReady) return;
    bool changed = !s_frameValid;
    for (uint8_t i = 0; i < count && !changed; i++) changed = colors[i] != s_frame[i];
    if (!changed) return;

    size_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint32_t c = colors[i];
        // WS2812B prima redosled G, R, B, najviši bit prvi
        uint32_t grb = (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c & 0xFF);
        for (int bit = 23; bit >= 0; bit--) {
            bool one = grb & (1UL << bit);
            s_symbols[n].level0 = 1;
            s_symbols[n].duration0 = one ? T1H : T0H;
            s_symbols[n].level1 = 0;
            s_symbols[n].duration1 = one ? T1L : T0L;
            n++;
        }
        s_frame[i] = c;
    }
    s_frameValid = true;
    rmtWrite(PIN_WS2812B, s_symbols, n, 100);  // ~30 µs po diodi; posle upisa linija ostaje niska (reset)
}

static void allOff(uint8_t count) {
    uint32_t off[LED_MAX_COUNT] = {0};
    show(off, count);
}

// ---------------------------------------------------------------------------
// Osvetljenje: podešeno -> noćni režim -> auto (LTR-390) -> ograničenje struje
// ---------------------------------------------------------------------------

static bool inNightWindow() {
    const DeviceConfig &c = config_manager::get();
    if (!c.ledNightEnabled || !time_manager::isValid()) return false;  // bez pouzdanog vremena nema noćnog režima
    time_t t = time_manager::nowUtc();
    struct tm tmv;
    localtime_r(&t, &tmv);
    int h = tmv.tm_hour;
    return c.ledNightStart > c.ledNightEnd ? (h >= c.ledNightStart || h < c.ledNightEnd)
                                           : (h >= c.ledNightStart && h < c.ledNightEnd);
}

static float brightnessFactor(uint32_t now, const uint32_t *colors, uint8_t count) {
    const DeviceConfig &c = config_manager::get();
    float pct = c.ledBrightness;
    s_night = inNightWindow();
    if (s_night) pct = min<float>(pct, c.ledNightBrightness);
    if (c.ledAutoBrightness && metrics::isFresh(MetricId::Illuminance, now)) {
        float lux = metrics::get(MetricId::Illuminance).value;
        pct *= constrain(lux / 300.0f, 0.15f, 1.0f);  // u mraku tamnije, na dnevnom svetlu puna podešena jačina
    }
    // Procena struje: ~20 mA po kanalu pri punoj jačini (specifikacija §10A.4)
    float ma = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint32_t col = colors[i];
        ma += (((col >> 16) & 0xFF) + ((col >> 8) & 0xFF) + (col & 0xFF)) / 255.0f * LED_MA_PER_CHANNEL;
    }
    ma *= pct / 100.0f;
    if (ma > LED_CURRENT_LIMIT_MA) pct *= LED_CURRENT_LIMIT_MA / ma;
    s_effectiveBrightness = (uint8_t)(pct + 0.5f);
    return pct / 100.0f;
}

// ---------------------------------------------------------------------------

static uint8_t sourceLevel() {
    switch (config_manager::get().ledSource) {
        case 1: return air_quality::levelOf(MetricId::Pm2_5);
        case 2: return air_quality::levelOf(MetricId::Pm10);
        default: return air_quality::level();
    }
}

static LedState decideState(uint32_t now) {
    if (ota_manager::inProgress()) return LedState::Ota;
    if ((int32_t)(s_bootTestUntilMs - now) > 0) return LedState::BootTest;
    if (wifi_manager::portalActive()) return LedState::Portal;
    s_level = sourceLevel();
    if (s_level > 0) return LedState::Level;
    // Nevalidno ili zastarelo PM merenje se nikada ne prikazuje bojom "dobrog" vazduha (§10A.2)
    return metrics::get(MetricId::Pm2_5).error == ErrorCode::Warmup ? LedState::Warmup : LedState::SensorFault;
}

void update(uint32_t now) {
    const DeviceConfig &cfg = config_manager::get();

    if (s_saveAtMs != 0 && (int32_t)(now - s_saveAtMs) >= 0) {
        s_saveAtMs = 0;
        config_manager::save();
    }

    if (!cfg.ledEnabled) {
        if (s_state != LedState::Off) {
            s_state = LedState::Off;
            allOff(LED_MAX_COUNT);  // gase se i diode iznad trenutno podešenog broja
        }
        return;
    }
    if (now - s_lastFrameMs < LED_FRAME_MS) return;
    s_lastFrameMs = now;
    ensureRmt();

    LedState st = decideState(now);
    if (st != s_state) {
        s_state = st;
        s_stateSinceMs = now;
        LOG_D(TAG, "Stanje: %s", stateKey(st));
    }
    uint32_t t = now - s_stateSinceMs;
    uint8_t count = cfg.ledCount;
    uint32_t colors[LED_MAX_COUNT] = {0};
    float intensity = 1.0f;
    uint32_t base = 0;

    switch (st) {
        case LedState::BootTest: {
            uint32_t phase = (now - (s_bootTestUntilMs - 1500)) / 500;
            base = phase == 0 ? 0xFF0000 : (phase == 1 ? 0x00FF00 : 0x0000FF);
            break;
        }
        case LedState::Level: {
            uint32_t target = LEVEL_COLORS[s_level];
            if (target != s_toColor) {  // blag prelaz na novu boju
                s_fromColor = s_shownColor;
                s_toColor = target;
                s_fadeStartMs = now;
            }
            base = mix(s_fromColor, s_toColor, (float)(now - s_fadeStartMs) / LED_FADE_MS);
            if (cfg.ledAlarmPulse && s_level >= 4) {
                intensity = 0.35f + 0.65f * (0.5f + 0.5f * sinf(2 * PI * (now % 2000) / 2000.0f));
            }
            break;
        }
        case LedState::Warmup:  // plava, sporo pulsiranje
            base = 0x0040FF;
            intensity = 0.15f + 0.85f * (0.5f + 0.5f * sinf(2 * PI * (t % 3000) / 3000.0f));
            break;
        case LedState::SensorFault:  // bela, slaba, sporo treperenje
            base = 0xFFFFFF;
            intensity = (t % 3000) < 300 ? 0.4f : 0.0f;
            break;
        case LedState::Portal: {  // plava, dvostruki bljesak
            uint32_t p = t % 2000;
            base = 0x0000FF;
            intensity = (p < 150 || (p >= 300 && p < 450)) ? 1.0f : 0.0f;
            break;
        }
        case LedState::Ota:  // naizmenično ljubičasto i plavo
            base = ((t / 250) % 2) ? 0x8000FF : 0x0040FF;
            break;
        case LedState::Off:
            break;
    }
    s_shownColor = base;

    if (st == LedState::Level && cfg.ledMode == 1 && count > 1) {
        // Bar-prikaz: broj upaljenih dioda srazmeran nivou
        uint8_t lit = (uint8_t)ceilf((float)s_level / 6.0f * count);
        for (uint8_t i = 0; i < count; i++) colors[i] = i < lit ? scale(base, intensity) : 0;
    } else if (st == LedState::Ota && count > 1) {
        for (uint8_t i = 0; i < count; i++) colors[i] = (i == (t / 120) % count) ? base : 0;  // kruženje
    } else {
        for (uint8_t i = 0; i < count; i++) colors[i] = scale(base, intensity);
    }

    float k = brightnessFactor(now, colors, count);
    for (uint8_t i = 0; i < count; i++) colors[i] = scale(colors[i], k);
    show(colors, count);
}

void begin() {
    const DeviceConfig &cfg = config_manager::get();
    if (cfg.ledEnabled) {
        ensureRmt();
        s_bootTestUntilMs = millis() + 1500;  // test boja: crvena -> zelena -> plava
        LOG_I(TAG, "LED indikator: %u dioda na GPIO%d", cfg.ledCount, PIN_WS2812B);
    } else {
        LOG_I(TAG, "LED indikator je isključen u podešavanjima");
    }
}

void applyConfig() {
    const DeviceConfig &cfg = config_manager::get();
    s_frameValid = false;  // sledeći kadar se šalje bez obzira na prethodni
    if (cfg.ledEnabled && s_state == LedState::Off) {
        ensureRmt();
        s_bootTestUntilMs = millis() + 1500;
    }
    if (s_rmtReady) allOff(LED_MAX_COUNT);  // gasi diode koje više nisu u upotrebi (manji broj dioda)
    s_frameValid = false;
}

void setFromHomeAssistant(bool on, int brightness255) {
    DeviceConfig &cfg = config_manager::edit();
    cfg.ledEnabled = on;
    if (brightness255 >= 0) cfg.ledBrightness = (uint8_t)constrain((brightness255 * 100 + 127) / 255, 1, 100);
    s_saveAtMs = millis() + 5000;
    applyConfig();
    LOG_I(TAG, "Home Assistant: LED %s, osvetljenje %u %%", on ? "uključen" : "isključen", cfg.ledBrightness);
}

LedState state() { return s_state; }

const char *stateKey(LedState s) {
    switch (s) {
        case LedState::Off:         return "off";
        case LedState::BootTest:    return "boot_test";
        case LedState::Level:       return "level";
        case LedState::Warmup:      return "warmup";
        case LedState::SensorFault: return "sensor_fault";
        case LedState::Portal:      return "portal";
        case LedState::Ota:         return "ota";
    }
    return "off";
}

uint32_t currentColor() { return s_state == LedState::Off ? 0 : s_shownColor; }

uint8_t effectiveBrightness() { return s_state == LedState::Off ? 0 : s_effectiveBrightness; }

uint8_t level() { return s_level; }

bool nightActive() { return s_night; }

}  // namespace led_manager
