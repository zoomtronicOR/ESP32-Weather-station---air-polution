#include "alarm_manager.h"

#include <Preferences.h>

#include "config.h"
#include "log.h"
#include "metrics.h"
#include "mqtt_manager.h"
#include "sensor_manager.h"
#include "storage_manager.h"
#include "time_manager.h"
#include "wifi_manager.h"

namespace alarm_manager {

static const char *TAG = "alarm";

// ---------------------------------------------------------------------------
// Pravila. Pragovi su korisnički (menjaju se u web interfejsu); kolona reference navodi
// zvaničnu skalu iz koje je podrazumevana vrednost preuzeta, ako postoji.
// ---------------------------------------------------------------------------

enum RuleIdx : uint8_t {
    R_TEMP_HIGH, R_TEMP_LOW, R_HUMIDITY_HIGH, R_PRESSURE_CHANGE, R_PM25_HIGH, R_PM10_HIGH, R_UV_HIGH,
    R_SENSOR_FAULT, R_WIFI_LOST, R_MQTT_LOST, R_TIME_SYNC, R_HEAP_LOW, R_COUNT
};

static const RuleInfo INFOS[R_COUNT] = {
    {"temp_high", "Visoka temperatura", Kind::Above, "°C", 1, "korisnički prag"},
    {"temp_low", "Niska temperatura", Kind::Below, "°C", 1, "korisnički prag"},
    {"humidity_high", "Visoka vlažnost", Kind::Above, "%", 0, "korisnički prag"},
    {"pressure_change", "Brza promena pritiska (3 h)", Kind::Change3h, "hPa", 1, "korisnički prag"},
    {"pm25_high", "Povišen PM2.5", Kind::Above, "µg/m³", 0, "EAQI: od 50 µg/m³ nivo 'Loš'"},
    {"pm10_high", "Povišen PM10", Kind::Above, "µg/m³", 0, "EAQI: od 120 µg/m³ nivo 'Loš'"},
    {"uv_high", "Visok UV indeks", Kind::Above, "", 1, "WHO: od 8 'vrlo visok'"},
    {"sensor_fault", "Senzor nedostupan", Kind::Condition, "", 0, "stanje uređaja"},
    {"wifi_lost", "Nema Wi-Fi veze", Kind::Condition, "", 0, "stanje uređaja"},
    {"mqtt_lost", "Nema MQTT veze", Kind::Condition, "", 0, "stanje uređaja"},
    {"time_sync", "Vreme nije sinhronizovano", Kind::Condition, "", 0, "specifikacija §12.9"},
    {"heap_low", "Malo slobodne memorije", Kind::Below, "KB", 0, "korisnički prag"},
};

static const RuleConfig DEFAULTS[R_COUNT] = {
    {true, 35.0f, 1.0f, 120},    // temp_high
    {true, 0.0f, 1.0f, 120},     // temp_low
    {true, 80.0f, 3.0f, 300},    // humidity_high
    {true, 3.0f, 0.5f, 0},       // pressure_change
    {true, 50.0f, 5.0f, 300},    // pm25_high
    {true, 120.0f, 10.0f, 300},  // pm10_high
    {true, 8.0f, 1.0f, 120},     // uv_high
    {true, 0.0f, 0.0f, 60},      // sensor_fault
    {true, 0.0f, 0.0f, 120},     // wifi_lost
    {true, 0.0f, 0.0f, 120},     // mqtt_lost
    {true, 0.0f, 0.0f, 900},     // time_sync: 15 min (specifikacija §12.9)
    {true, 30.0f, 5.0f, 60},     // heap_low
};

static const MetricId METRIC_OF[R_COUNT] = {
    MetricId::Temperature, MetricId::Temperature, MetricId::Humidity, MetricId::Pressure, MetricId::Pm2_5,
    MetricId::Pm10, MetricId::UvIndex, MetricId::Count, MetricId::Count, MetricId::Count, MetricId::Count,
    MetricId::Count,
};

static RuleConfig s_cfg[R_COUNT];
static RuleStatus s_st[R_COUNT];
static uint32_t s_lastEvalMs = 0;
static uint32_t s_changeCounter = 0;

static constexpr uint32_t CFG_VERSION = 1;

// Red događaja za MQTT
static Event s_queue[8];
static size_t s_qHead = 0, s_qCount = 0;

// Pritisak pre 3 h iz 5-minutne istorije (keš, osvežava se na minut)
static float s_pressure3h = NAN;
static uint32_t s_pressureCheckMs = 0;

// ---------------------------------------------------------------------------
// Fajl događaja na particiji istorije: zaglavlje + kružni niz
// ---------------------------------------------------------------------------

static const char *EVENTS_PATH = "/alarms.bin";
struct __attribute__((packed)) EvHeader {
    uint32_t magic;
    uint16_t head;
    uint16_t count;
};
static constexpr uint32_t EV_MAGIC = 0x414C5231;  // "ALR1"
static EvHeader s_ev = {EV_MAGIC, 0, 0};
static SemaphoreHandle_t s_evMutex = nullptr;

static void openEvents() {
    if (!storage_manager::fsReady()) return;
    fs::FS &fs = storage_manager::fs();
    File f = fs.open(EVENTS_PATH, "r");
    size_t expected = sizeof(EvHeader) + ALARM_EVENTS_MAX * sizeof(Event);
    if (f && f.size() == expected && f.read((uint8_t *)&s_ev, sizeof(s_ev)) == sizeof(s_ev) && s_ev.magic == EV_MAGIC &&
        s_ev.head < ALARM_EVENTS_MAX && s_ev.count <= ALARM_EVENTS_MAX) {
        f.close();
        return;
    }
    if (f) f.close();
    f = fs.open(EVENTS_PATH, "w");
    if (!f) return;
    s_ev = {EV_MAGIC, 0, 0};
    f.write((const uint8_t *)&s_ev, sizeof(s_ev));
    Event empty = {};
    for (size_t i = 0; i < ALARM_EVENTS_MAX; i++) f.write((const uint8_t *)&empty, sizeof(empty));
    f.close();
}

static void storeEvent(const Event &e) {
    // Red za MQTT (najstariji se odbacuje ako je pun)
    s_queue[(s_qHead + s_qCount) % 8] = e;
    if (s_qCount < 8) s_qCount++;
    else s_qHead = (s_qHead + 1) % 8;

    if (!storage_manager::fsReady()) return;
    xSemaphoreTake(s_evMutex, portMAX_DELAY);
    File f = storage_manager::fs().open(EVENTS_PATH, "r+");
    if (f) {
        f.seek(sizeof(EvHeader) + (size_t)s_ev.head * sizeof(Event));
        f.write((const uint8_t *)&e, sizeof(e));
        s_ev.head = (s_ev.head + 1) % ALARM_EVENTS_MAX;
        if (s_ev.count < ALARM_EVENTS_MAX) s_ev.count++;
        f.seek(0);
        f.write((const uint8_t *)&s_ev, sizeof(s_ev));
        f.close();
    }
    xSemaphoreGive(s_evMutex);
}

static void addEvent(size_t rule, EventType type, float value) {
    Event e;
    e.utc = time_manager::isValid() ? (uint32_t)time_manager::nowUtc() : 0;
    e.uptimeMs = millis();
    e.rule = rule;
    e.type = (uint8_t)type;
    e.value = value;
    storeEvent(e);
    s_changeCounter++;
}

// ---------------------------------------------------------------------------
// Konfiguracija u NVS
// ---------------------------------------------------------------------------

static void loadConfig() {
    memcpy(s_cfg, DEFAULTS, sizeof(s_cfg));
    Preferences p;
    if (!p.begin("alarms", true)) return;
    if (p.getUInt("ver", 0) == CFG_VERSION && p.getBytesLength("cfg") == sizeof(s_cfg)) {
        p.getBytes("cfg", s_cfg, sizeof(s_cfg));
    }
    p.end();
}

void saveConfig() {
    Preferences p;
    if (!p.begin("alarms", false)) {
        LOG_E(TAG, "Upis podešavanja alarma nije uspeo");
        return;
    }
    p.putUInt("ver", CFG_VERSION);
    p.putBytes("cfg", s_cfg, sizeof(s_cfg));
    p.end();
    LOG_I(TAG, "Podešavanja alarma su sačuvana");
}

// ---------------------------------------------------------------------------
// Procena uslova
// ---------------------------------------------------------------------------

static void refreshPressure3h(uint32_t now) {
    if (s_pressureCheckMs != 0 && now - s_pressureCheckMs < 60000) return;
    s_pressureCheckMs = now;
    s_pressure3h = NAN;
    if (!time_manager::isValid()) return;
    uint32_t target = (uint32_t)time_manager::nowUtc() - 3 * 3600;
    size_t n = storage_manager::count(storage_manager::Resolution::Min5);
    if (n == 0) return;
    // Zapis najbliži trenutku pre 3 h (prihvata se odstupanje do 10 min)
    storage_manager::HistRecord recs[4];
    size_t start = n > 40 ? n - 40 : 0;
    uint32_t bestDiff = 600;
    for (size_t i = start; i < n; i += 4) {
        size_t got = storage_manager::readRange(storage_manager::Resolution::Min5, i, recs, 4);
        for (size_t k = 0; k < got; k++) {
            const auto &a = recs[k].m[(size_t)MetricId::Pressure];
            uint32_t mid = recs[k].ts + 150;
            uint32_t diff = mid > target ? mid - target : target - mid;
            if (a.count && diff <= bestDiff) {
                bestDiff = diff;
                s_pressure3h = a.avg;
            }
        }
    }
}

// valid = uslov se može proceniti; value = vrednost za prikaz/poređenje
static bool evaluate(size_t i, uint32_t now, float &value, bool &exceed, bool &clear) {
    const RuleConfig &c = s_cfg[i];
    value = 0;
    switch (i) {
        case R_SENSOR_FAULT: {
            sensor_manager::Health h = sensor_manager::health();
            value = h.faulty;
            exceed = h.faulty > 0;
            clear = !exceed;
            return true;
        }
        case R_WIFI_LOST:
            if (!wifi_manager::hasCredentials()) return false;
            exceed = !wifi_manager::connected();
            clear = !exceed;
            return true;
        case R_MQTT_LOST:
            if (!mqtt_manager::enabled() || !wifi_manager::connected()) {
                exceed = false;
                clear = true;  // bez Wi-Fi-ja se prijavljuje Wi-Fi alarm, ne MQTT
                return true;
            }
            exceed = !mqtt_manager::connected();
            clear = !exceed;
            return true;
        case R_TIME_SYNC: {
            auto st = time_manager::status();
            bool bad = st == time_manager::TimeStatus::Stale ||
                       (wifi_manager::connected() && !time_manager::isValid());
            exceed = bad;
            clear = !bad;
            return true;
        }
        case R_HEAP_LOW:
            value = ESP.getFreeHeap() / 1024.0f;
            exceed = value < c.threshold;
            clear = value >= c.threshold + c.hysteresis;
            return true;
        case R_PRESSURE_CHANGE: {
            refreshPressure3h(now);
            if (!metrics::isFresh(MetricId::Pressure, now) || isnan(s_pressure3h)) return false;
            value = metrics::get(MetricId::Pressure).value - s_pressure3h;
            exceed = fabsf(value) >= c.threshold;
            clear = fabsf(value) < c.threshold - c.hysteresis;
            return true;
        }
        default: {
            MetricId m = METRIC_OF[i];
            if (!metrics::isFresh(m, now)) return false;  // nevalidno merenje ne menja stanje alarma
            value = metrics::get(m).value;
            if (INFOS[i].kind == Kind::Above) {
                exceed = value > c.threshold;
                clear = value <= c.threshold - c.hysteresis;
            } else {
                exceed = value < c.threshold;
                clear = value >= c.threshold + c.hysteresis;
            }
            return true;
        }
    }
}

static bool worse(size_t i, float a, float b) {
    switch (INFOS[i].kind) {
        case Kind::Below: return a < b;
        case Kind::Change3h: return fabsf(a) > fabsf(b);
        default: return a > b;
    }
}

static void resolve(size_t i, float value) {
    RuleStatus &s = s_st[i];
    s.state = State::Inactive;
    LOG_I(TAG, "Razrešen: %s", INFOS[i].name);
    addEvent(i, EventType::Resolved, value);
}

void update(uint32_t now) {
    if (now - s_lastEvalMs < ALARM_EVAL_MS) return;
    s_lastEvalMs = now;

    for (size_t i = 0; i < R_COUNT; i++) {
        RuleStatus &s = s_st[i];
        if (!s_cfg[i].enabled) {
            if (s.state == State::Active) resolve(i, s.value);
            s.state = State::Inactive;
            continue;
        }
        float value;
        bool exceed = false, clear = false;
        if (!evaluate(i, now, value, exceed, clear)) continue;
        s.value = value;
        s.hasValue = true;

        switch (s.state) {
            case State::Inactive:
                if (exceed) {
                    s.state = State::Pending;
                    s.sinceMs = now;
                }
                if (s.state != State::Pending || s_cfg[i].minDurationS != 0) break;
                // bez minimalnog trajanja alarm se aktivira odmah
                [[fallthrough]];
            case State::Pending:
                if (!exceed) {
                    s.state = State::Inactive;
                } else if (now - s.sinceMs >= (uint32_t)s_cfg[i].minDurationS * 1000) {
                    s.state = State::Active;
                    s.acknowledged = false;
                    s.activatedMs = now;
                    s.activatedUtc = time_manager::isValid() ? (uint32_t)time_manager::nowUtc() : 0;
                    s.peak = value;
                    LOG_W(TAG, "Aktiviran: %s (vrednost %.*f %s)", INFOS[i].name, INFOS[i].decimals, value, INFOS[i].unit);
                    addEvent(i, EventType::Activated, value);
                }
                break;
            case State::Active:
                if (worse(i, value, s.peak)) s.peak = value;
                if (clear) resolve(i, value);
                break;
        }
    }
}

// ---------------------------------------------------------------------------

void begin() {
    s_evMutex = xSemaphoreCreateMutex();
    loadConfig();
    for (auto &s : s_st) s = {State::Inactive, false, 0, 0, 0, NAN, NAN, false};
    openEvents();
    LOG_I(TAG, "Alarmi: %u pravila, %u sačuvanih događaja", (unsigned)R_COUNT, s_ev.count);
}

size_t ruleCount() { return R_COUNT; }

const RuleInfo &info(size_t i) { return INFOS[i]; }

const RuleConfig &config(size_t i) { return s_cfg[i]; }

const RuleStatus &status(size_t i) { return s_st[i]; }

int find(const char *id) {
    for (size_t i = 0; i < R_COUNT; i++) {
        if (strcmp(INFOS[i].id, id) == 0) return i;
    }
    return -1;
}

size_t activeCount() {
    size_t n = 0;
    for (const auto &s : s_st) n += s.state == State::Active;
    return n;
}

void setConfig(size_t i, const RuleConfig &cfg) {
    if (i >= R_COUNT) return;
    s_cfg[i] = cfg;
    s_changeCounter++;
}

void acknowledge(int i) {
    for (size_t k = 0; k < R_COUNT; k++) {
        if ((i < 0 || (size_t)i == k) && s_st[k].state == State::Active && !s_st[k].acknowledged) {
            s_st[k].acknowledged = true;
            addEvent(k, EventType::Acknowledged, s_st[k].value);
        }
    }
}

void clearEvents() {
    if (!storage_manager::fsReady()) return;
    xSemaphoreTake(s_evMutex, portMAX_DELAY);
    storage_manager::fs().remove(EVENTS_PATH);
    openEvents();
    xSemaphoreGive(s_evMutex);
    LOG_I(TAG, "Istorija alarma je obrisana");
}

void forEachEvent(size_t max, const std::function<void(const Event &)> &fn) {
    if (!storage_manager::fsReady() || !s_evMutex) return;
    xSemaphoreTake(s_evMutex, portMAX_DELAY);
    File f = storage_manager::fs().open(EVENTS_PATH, "r");
    if (f) {
        size_t n = min<size_t>(max, s_ev.count);
        for (size_t k = 0; k < n; k++) {
            size_t slot = (s_ev.head + ALARM_EVENTS_MAX - 1 - k) % ALARM_EVENTS_MAX;
            Event e;
            if (f.seek(sizeof(EvHeader) + slot * sizeof(Event)) && f.read((uint8_t *)&e, sizeof(e)) == sizeof(e)) fn(e);
        }
        f.close();
    }
    xSemaphoreGive(s_evMutex);
}

uint32_t changeCounter() { return s_changeCounter; }

bool popEvent(Event &out) {
    if (s_qCount == 0) return false;
    out = s_queue[s_qHead];
    s_qHead = (s_qHead + 1) % 8;
    s_qCount--;
    return true;
}

const char *stateKey(State s) {
    switch (s) {
        case State::Inactive: return "inactive";
        case State::Pending:  return "pending";
        case State::Active:   return "active";
    }
    return "inactive";
}

const char *kindKey(Kind k) {
    switch (k) {
        case Kind::Above:     return "above";
        case Kind::Below:     return "below";
        case Kind::Change3h:  return "change3h";
        case Kind::Condition: return "condition";
    }
    return "above";
}

}  // namespace alarm_manager
