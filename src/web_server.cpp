#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <sys/time.h>

#include "air_quality.h"
#include "config.h"
#include "config_manager.h"
#include "diagnostics.h"
#include "display_manager.h"
#include "i2c_bus.h"
#include "log.h"
#include "metrics.h"
#include "pins.h"
#include "sensor_manager.h"
#include "status_led.h"
#include "time_manager.h"
#include "wifi_manager.h"

namespace web_server {

static const char *TAG = "web";

static AsyncWebServer s_server(80);

// ---------------------------------------------------------------------------
// Akcije iz web zadatka -> glavna petlja
// ---------------------------------------------------------------------------

enum Action : uint32_t {
    ACT_SAVE_CONFIG = 1 << 0,
    ACT_WIFI_RECONNECT = 1 << 1,
    ACT_TIME_CONFIG = 1 << 2,
    ACT_SET_TIME = 1 << 3,
    ACT_PORTAL = 1 << 4,
    ACT_RESTART = 1 << 5,
    ACT_DISPLAY = 1 << 6,
};

static SemaphoreHandle_t s_mutex = nullptr;
static uint32_t s_pending = 0;
static DeviceConfig s_pendingConfig;
static time_t s_pendingEpoch = 0;
static uint32_t s_restartAtMs = 0;

// Vraća kopiju konfiguracije u koju handler upisuje izmene (poziva se pod mutex-om)
static DeviceConfig &pendingConfig() {
    if (!(s_pending & ACT_SAVE_CONFIG)) {
        s_pendingConfig = config_manager::get();
        s_pending |= ACT_SAVE_CONFIG;
    }
    return s_pendingConfig;
}

void update(uint32_t now) {
    if (s_restartAtMs != 0 && (int32_t)(now - s_restartAtMs) >= 0) {
        LOG_W(TAG, "Restart na zahtev iz web interfejsa");
        delay(100);
        ESP.restart();
    }
    if (s_pending == 0 || xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return;
    }
    uint32_t actions = s_pending;
    s_pending = 0;
    if (actions & ACT_SAVE_CONFIG) {
        config_manager::edit() = s_pendingConfig;
    }
    time_t epoch = s_pendingEpoch;
    xSemaphoreGive(s_mutex);

    if (actions & ACT_SAVE_CONFIG) config_manager::save();
    if (actions & ACT_TIME_CONFIG) time_manager::applyConfig();
    if (actions & ACT_DISPLAY) display_manager::applyBrightness();
    if (actions & ACT_SET_TIME) time_manager::setManual(epoch);
    if (actions & ACT_WIFI_RECONNECT) wifi_manager::reconnect(now);
    if (actions & ACT_PORTAL) wifi_manager::startPortal(now);
    if (actions & ACT_RESTART) s_restartAtMs = now + 1000;  // odgovor stigne do pregledača pre restarta
}

// ---------------------------------------------------------------------------
// Pomoćne funkcije
// ---------------------------------------------------------------------------

static void sendJson(AsyncWebServerRequest *req, JsonDocument &doc, int code = 200) {
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->setCode(code);
    res->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *res);
    req->send(res);
}

static void sendError(AsyncWebServerRequest *req, int code, const char *message) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = message;
    sendJson(req, doc, code);
}

static void sendOk(AsyncWebServerRequest *req, const char *message) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["message"] = message;
    sendJson(req, doc);
}

// Vrednost zaokružena na zadati broj decimala; ključ se zadaje posebno, jer MemberProxy
// pretvoren u JsonVariant za još nepostojeći ključ ne pravi novi član
static void setRounded(JsonObject obj, const char *key, float value, uint8_t decimals) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    obj[key] = serialized(String(buf));
}

static void setTimestamp(JsonObject obj, const char *key, uint32_t uptimeMs) {
    if (time_manager::isValid()) {
        char buf[24];
        time_manager::formatIsoUtc(time_manager::epochFromUptime(uptimeMs), buf, sizeof(buf));
        obj[key] = buf;
    } else {
        obj[key] = nullptr;
    }
}

static void ipString(uint32_t ip, char *buf, size_t len) { strlcpy(buf, IPAddress(ip).toString().c_str(), len); }

// Oznaka pina na ploči NodeMCU ESP-32S (npr. GPIO21 -> "P21", GPIO36 -> "SVP")
static void addPin(JsonArray pins, const char *role, uint8_t gpio) {
    JsonObject p = pins.add<JsonObject>();
    p["role"] = role;
    p["gpio"] = gpio;
    char label[8];
    if (gpio == 36) strlcpy(label, "SVP", sizeof(label));
    else if (gpio == 39) strlcpy(label, "SVN", sizeof(label));
    else snprintf(label, sizeof(label), "P%u", gpio);
    p["label"] = label;
}

// ---------------------------------------------------------------------------
// Autentifikacija (specifikacija §14): HTTP Basic, ograničen broj neuspešnih pokušaja
// ---------------------------------------------------------------------------

struct AuthSlot {
    uint32_t ip;
    uint8_t failures;
    uint32_t lockedUntilMs;
    uint32_t lastSeenMs;
};
static AuthSlot s_authSlots[4];

static AuthSlot &authSlot(uint32_t ip) {
    AuthSlot *oldest = &s_authSlots[0];
    for (AuthSlot &s : s_authSlots) {
        if (s.ip == ip) return s;
        if (s.lastSeenMs < oldest->lastSeenMs) oldest = &s;
    }
    *oldest = {ip, 0, 0, 0};
    return *oldest;
}

// Klijent povezan na konfiguracionu pristupnu tačku (ona je već zaštićena lozinkom)
static bool isPortalClient(AsyncWebServerRequest *req) {
    return wifi_manager::portalActive() && req->client()->localIP() == WiFi.softAPIP();
}

static bool requireAuth(AsyncWebServerRequest *req) {
    uint32_t now = millis();
    AuthSlot &slot = authSlot((uint32_t)req->client()->remoteIP());
    slot.lastSeenMs = now;
    if (slot.lockedUntilMs != 0 && (int32_t)(slot.lockedUntilMs - now) > 0) {
        sendError(req, 429, "Previše neuspešnih pokušaja prijave. Pokušajte ponovo za nekoliko minuta.");
        return false;
    }
    if (req->authenticate(WEB_ADMIN_USER, config_manager::adminPassword())) {
        slot.failures = 0;
        slot.lockedUntilMs = 0;
        return true;
    }
    if (++slot.failures >= AUTH_MAX_FAILURES) {
        slot.failures = 0;
        slot.lockedUntilMs = now + AUTH_LOCKOUT_MS;
        LOG_W(TAG, "Prijava zaključana na %lu s za %s", (unsigned long)(AUTH_LOCKOUT_MS / 1000),
              req->client()->remoteIP().toString().c_str());
    }
    // Bez zaglavlja WWW-Authenticate, da pregledač ne otvara svoj prozor za prijavu
    sendError(req, 401, "Potrebna je administratorska lozinka.");
    return false;
}

// ---------------------------------------------------------------------------
// GET handleri
// ---------------------------------------------------------------------------

static void addTimeInfo(JsonObject t) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    char buf[32];
    t["status"] = time_manager::statusKey(time_manager::status());
    t["valid"] = time_manager::isValid();
    t["epoch"] = (int64_t)tv.tv_sec;
    t["epoch_ms"] = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    time_manager::formatIsoUtc(tv.tv_sec, buf, sizeof(buf));
    t["utc"] = buf;
    time_manager::formatIsoLocal(tv.tv_sec, buf, sizeof(buf));
    t["local"] = buf;
    t["timezone"] = config_manager::get().timezone;
    t["tz_abbrev"] = time_manager::tzAbbrev();
}

static void handleStatus(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["device"] = DEVICE_NAME;
    doc["firmware"] = FW_VERSION;
    doc["uptime_s"] = millis() / 1000;
    addTimeInfo(doc["time"].to<JsonObject>());

    ConnectionInfo ci = wifi_manager::connectionInfo();
    JsonObject w = doc["wifi"].to<JsonObject>();
    w["connected"] = wifi_manager::connected();
    w["portal"] = wifi_manager::portalActive();
    w["ssid"] = ci.ssid;
    w["ip"] = ci.ip;
    w["rssi"] = ci.rssi;

    sensor_manager::Health h = sensor_manager::health();
    JsonObject s = doc["sensors"].to<JsonObject>();
    s["total"] = h.total;
    s["ok"] = h.ok;
    s["initializing"] = h.initializing;
    s["faulty"] = h.faulty;

    doc["status_led"] = status_led::describe(status_led::current());
    doc["admin_password_set"] = config_manager::adminPasswordSet();
    sendJson(req, doc);
}

static void handleReadings(AsyncWebServerRequest *req) {
    uint32_t now = millis();
    JsonDocument doc;
    if (time_manager::isValid()) {
        char buf[24];
        time_manager::formatIsoUtc(time_manager::nowUtc(), buf, sizeof(buf));
        doc["timestamp"] = buf;
    } else {
        doc["timestamp"] = nullptr;
    }
    doc["time_valid"] = time_manager::isValid();

    JsonArray list = doc["metrics"].to<JsonArray>();
    JsonObject grouped = doc["sensors"].to<JsonObject>();

    for (size_t i = 0; i < (size_t)MetricId::Count; i++) {
        MetricId id = (MetricId)i;
        const MetricDef &d = metrics::def(id);
        const Reading &r = metrics::get(id);
        bool fresh = metrics::isFresh(id, now);

        JsonObject m = list.add<JsonObject>();
        m["key"] = d.key;
        m["name"] = d.name;
        m["unit"] = d.unit;
        m["decimals"] = d.decimals;
        m["source"] = d.source;
        if (fresh) {
            setRounded(m, "value", r.value, d.decimals);
            m["state"] = "ok";
        } else {
            m["value"] = nullptr;
            bool unknown = !r.everUpdated && r.error == ErrorCode::None;
            m["state"] = unknown ? "unknown" : "unavailable";
            if (!unknown) m["reason"] = r.valid ? "zastarelo" : errorCodeText(r.error);
        }
        if (r.everUpdated) {
            m["age_s"] = (now - r.timestampMs) / 1000;
            setTimestamp(m, "timestamp", r.timestampMs);
        }

        // Grupisano po senzoru, ključevi sa jedinicom (specifikacija §13)
        JsonObject g = grouped[d.source].is<JsonObject>() ? grouped[d.source].as<JsonObject>()
                                                          : grouped[d.source].to<JsonObject>();
        if (fresh) setRounded(g, d.apiKey, r.value, d.decimals);
        else g[d.apiKey] = nullptr;
        g["valid"] = (g["valid"].isNull() ? true : g["valid"].as<bool>()) && fresh;
    }

    uint8_t lvl = air_quality::level();
    JsonObject aq = doc["air_quality"].to<JsonObject>();
    aq["level"] = lvl;
    aq["name"] = air_quality::levelName(lvl);
    aq["scale"] = "EAQI";
    aq["local_estimate"] = true;
    if (lvl > 0) {
        aq["dominant"] = metrics::def(air_quality::dominant()).key;
        setRounded(aq, "dominant_avg_10min", air_quality::dominantAverage(), 1);
    }
    sendJson(req, doc);
}

static void addBusSensor(JsonArray list, const SensorDriver *d, uint32_t now) {
    JsonObject s = list.add<JsonObject>();
    s["id"] = d->id();
    s["name"] = d->name();
    s["interface"] = d->bus() == 0 ? "I2C0" : "I2C1";
    JsonArray pins = s["pins"].to<JsonArray>();
    if (d->bus() == 0) {
        addPin(pins, "SDA", PIN_I2C_SDA);
        addPin(pins, "SCL", PIN_I2C_SCL);
    } else {
        addPin(pins, "SDA", PIN_SPS30_SDA);
        addPin(pins, "SCL", PIN_SPS30_SCL);
    }
    char addr[8];
    snprintf(addr, sizeof(addr), "0x%02X", d->address());
    s["i2c_address"] = addr;
    s["supply_v"] = strcmp(d->id(), "sps30") == 0 ? 5.0 : 3.3;
    SensorStatus st = d->status();
    s["status"] = st == SensorStatus::Ok             ? "ok"
                  : st == SensorStatus::Initializing ? "initializing"
                  : st == SensorStatus::NotFound     ? "not_found"
                  : st == SensorStatus::Error        ? "error"
                                                     : "disabled";
    s["status_text"] = sensorStatusText(st);
    s["error_count"] = d->errorCount();
    if (d->lastOkMs() > 0) {
        s["last_reading_age_s"] = (now - d->lastOkMs()) / 1000;
        setTimestamp(s, "last_reading_utc", d->lastOkMs());
    }
    if (d->lastError() != ErrorCode::None) s["last_error"] = errorCodeText(d->lastError());
}

static void handleSensors(AsyncWebServerRequest *req) {
    uint32_t now = millis();
    JsonDocument doc;
    JsonArray list = doc["sensors"].to<JsonArray>();
    for (size_t i = 0; i < sensor_manager::count(); i++) {
        addBusSensor(list, sensor_manager::driver(i), now);
    }

    // Periferije (specifikacija §6.6)
    JsonArray per = doc["peripherals"].to<JsonArray>();
    JsonObject oled = per.add<JsonObject>();
    oled["id"] = "oled";
    oled["name"] = "OLED SSD1306 128×64 (dvobojni)";
    oled["interface"] = "I2C0";
    JsonArray op = oled["pins"].to<JsonArray>();
    addPin(op, "SDA", PIN_I2C_SDA);
    addPin(op, "SCL", PIN_I2C_SCL);
    oled["i2c_address"] = "0x3C";
    oled["supply_v"] = 3.3;
    oled["status"] = display_manager::available() ? "ok" : "not_found";
    oled["status_text"] = display_manager::available() ? "ispravan" : "nije pronađen";

    JsonObject led = per.add<JsonObject>();
    led["id"] = "status_led";
    led["name"] = "Statusna LED (ugrađena, plava)";
    led["interface"] = "GPIO";
    addPin(led["pins"].to<JsonArray>(), "LED", PIN_STATUS_LED);
    led["status"] = "ok";
    led["status_text"] = status_led::describe(status_led::current());

    JsonObject ws = per.add<JsonObject>();
    ws["id"] = "ws2812b";
    ws["name"] = "WS2812B LED indikator";
    ws["interface"] = "RMT";
    addPin(ws["pins"].to<JsonArray>(), "DIN", PIN_WS2812B);
    ws["supply_v"] = 5.0;
    ws["status"] = "disabled";
    ws["status_text"] = "nije povezan";

    // Upozorenje o pull-up otpornicima
    JsonObject buses = doc["buses"].to<JsonObject>();
    buses["i2c0_pullups_ok"] = i2c_bus::pullupsOk(Wire);
    buses["i2c1_pullups_ok"] = i2c_bus::pullupsOk(Wire1);
    sendJson(req, doc);
}

static void handleTime(AsyncWebServerRequest *req) {
    JsonDocument doc;
    addTimeInfo(doc.to<JsonObject>());
    const DeviceConfig &cfg = config_manager::get();
    doc["ntp_enabled"] = cfg.ntpEnabled;
    JsonArray servers = doc["ntp_servers"].to<JsonArray>();
    servers.add(cfg.ntpServer1);
    servers.add(cfg.ntpServer2);
    servers.add(cfg.ntpServer3);
    doc["ntp_interval_min"] = cfg.ntpIntervalMin;
    if (time_manager::lastSyncUtc() > 0) {
        char buf[24];
        time_manager::formatIsoUtc(time_manager::lastSyncUtc(), buf, sizeof(buf));
        doc["last_sync_utc"] = buf;
        doc["last_offset_ms"] = time_manager::lastOffsetMs();
    } else {
        doc["last_sync_utc"] = nullptr;
    }
    doc["sync_count"] = time_manager::syncCount();
    sendJson(req, doc);
}

static void handleWifi(AsyncWebServerRequest *req) {
    const DeviceConfig &cfg = config_manager::get();
    ConnectionInfo ci = wifi_manager::connectionInfo();
    JsonDocument doc;
    const char *mode = ci.mode == NetMode::Portal      ? "portal"
                       : ci.mode == NetMode::Connected  ? "connected"
                       : ci.mode == NetMode::Connecting ? "connecting"
                                                        : "off";
    doc["mode"] = mode;
    doc["connected"] = wifi_manager::connected();
    doc["credentials_set"] = wifi_manager::hasCredentials();
    doc["ssid"] = cfg.wifiSsid;
    doc["hostname"] = cfg.hostname;
    doc["mac"] = WiFi.macAddress();
    doc["static_ip"] = cfg.staticIp;
    char buf[16];
    if (wifi_manager::connected()) {
        doc["ip"] = WiFi.localIP().toString();
        doc["gateway"] = WiFi.gatewayIP().toString();
        doc["subnet"] = WiFi.subnetMask().toString();
        doc["dns"] = WiFi.dnsIP().toString();
        doc["rssi"] = WiFi.RSSI();
        doc["connected_for_s"] = (millis() - wifi_manager::connectedSinceMs()) / 1000;
    }
    JsonObject st = doc["static"].to<JsonObject>();
    ipString(cfg.ip, buf, sizeof(buf));      st["ip"] = buf;
    ipString(cfg.gateway, buf, sizeof(buf)); st["gateway"] = buf;
    ipString(cfg.subnet, buf, sizeof(buf));  st["subnet"] = buf;
    ipString(cfg.dns, buf, sizeof(buf));     st["dns"] = buf;
    doc["portal_active"] = wifi_manager::portalActive();
    doc["ap_ssid"] = WIFI_AP_SSID;
    doc["ap_ip"] = WiFi.softAPIP().toString();
    doc["reconnects"] = wifi_manager::reconnectCount();
    doc["portal_client"] = isPortalClient(req);
    sendJson(req, doc);
}

static void handleWifiScan(AsyncWebServerRequest *req) {
    int16_t n = WiFi.scanComplete();
    bool refresh = req->hasParam("refresh");
    JsonDocument doc;
    if (refresh || n == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true);  // asinhrono
        doc["scanning"] = true;
    } else if (n == WIFI_SCAN_RUNNING) {
        doc["scanning"] = true;
    } else {
        doc["scanning"] = false;
        JsonArray list = doc["networks"].to<JsonArray>();
        for (int16_t i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.isEmpty()) continue;
            bool duplicate = false;
            for (JsonObject o : list) {
                if (ssid == o["ssid"].as<const char *>()) { duplicate = true; break; }
            }
            if (duplicate) continue;
            JsonObject o = list.add<JsonObject>();
            o["ssid"] = ssid;
            o["rssi"] = WiFi.RSSI(i);
            o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            o["channel"] = WiFi.channel(i);
        }
    }
    sendJson(req, doc);
}

static void handleConfig(AsyncWebServerRequest *req) {
    const DeviceConfig &cfg = config_manager::get();
    JsonDocument doc;
    doc["hostname"] = cfg.hostname;
    doc["ntp_enabled"] = cfg.ntpEnabled;
    doc["ntp_server1"] = cfg.ntpServer1;
    doc["ntp_server2"] = cfg.ntpServer2;
    doc["ntp_server3"] = cfg.ntpServer3;
    doc["ntp_interval_min"] = cfg.ntpIntervalMin;
    doc["timezone"] = cfg.timezone;
    JsonArray tz = doc["timezones"].to<JsonArray>();
    for (size_t i = 0; i < config_manager::timezoneCount(); i++) tz.add(config_manager::timezoneName(i));
    doc["oled_brightness"] = cfg.oledBrightness;
    // Lozinke se nikada ne vraćaju -- samo informacija da li su podešene (specifikacija §13)
    doc["admin_password_set"] = config_manager::adminPasswordSet();
    doc["ap_password_custom"] = cfg.apPass[0] != '\0';
    sendJson(req, doc);
}

static void handleSystem(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["device"] = DEVICE_NAME;
    doc["firmware"] = FW_VERSION;
    doc["chip"] = ESP.getChipModel();
    uint16_t rev = ESP.getChipRevision();
    char revBuf[8];
    snprintf(revBuf, sizeof(revBuf), "v%u.%u", rev / 100, rev % 100);
    doc["chip_revision"] = revBuf;
    doc["cores"] = ESP.getChipCores();
    doc["cpu_mhz"] = ESP.getCpuFreqMHz();
    doc["flash_bytes"] = ESP.getFlashChipSize();
    doc["sdk"] = ESP.getSdkVersion();
    doc["arduino"] = ESP_ARDUINO_VERSION_STR;
    doc["uptime_s"] = millis() / 1000;
    doc["reset_reason"] = diagnostics::resetReasonText(diagnostics::resetReason());
    doc["boot_count"] = diagnostics::bootCount();
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_min_free"] = ESP.getMinFreeHeap();
    doc["heap_max_block"] = ESP.getMaxAllocHeap();
    doc["sketch_bytes"] = ESP.getSketchSize();
    doc["ota_partition_bytes"] = ESP.getFreeSketchSpace();
    doc["fs_total"] = LittleFS.totalBytes();
    doc["fs_used"] = LittleFS.usedBytes();
    doc["mac"] = WiFi.macAddress();
    doc["time_status"] = time_manager::statusKey(time_manager::status());
    sendJson(req, doc);
}

// ---------------------------------------------------------------------------
// POST handleri (zaštićeni lozinkom)
// ---------------------------------------------------------------------------

static bool validHostname(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 31 || h[0] == '-' || h[n - 1] == '-') return false;
    for (const char *p = h; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-')) return false;
    }
    return true;
}

static bool parseIp(JsonVariant v, uint32_t &out) {
    IPAddress ip;
    if (!v.is<const char *>() || !ip.fromString(v.as<const char *>())) return false;
    out = (uint32_t)ip;
    return true;
}

static bool lock() { return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE; }
static void unlock() { xSemaphoreGive(s_mutex); }

static void handleWifiPost(AsyncWebServerRequest *req, JsonVariant &json) {
    // Na portalu (pristupna tačka je zaštićena lozinkom) prvo podešavanje ne traži prijavu
    if (!isPortalClient(req) && !requireAuth(req)) return;

    const char *ssid = json["ssid"] | "";
    size_t ssidLen = strlen(ssid);
    if (ssidLen == 0 || ssidLen > 32) return sendError(req, 400, "Naziv mreže (SSID) mora imati 1--32 znaka.");
    const char *pass = json["password"] | (const char *)nullptr;
    if (pass && strlen(pass) > 64) return sendError(req, 400, "Lozinka mreže može imati najviše 64 znaka.");
    if (pass && strlen(pass) > 0 && strlen(pass) < 8) return sendError(req, 400, "Lozinka mreže mora imati bar 8 znakova.");

    bool staticIp = json["static_ip"] | false;
    uint32_t ip = 0, gw = 0, mask = 0, dns = 0;
    if (staticIp && !(parseIp(json["ip"], ip) && parseIp(json["gateway"], gw) && parseIp(json["subnet"], mask) &&
                      parseIp(json["dns"], dns))) {
        return sendError(req, 400, "Neispravna statička IP konfiguracija.");
    }
    const char *hostname = json["hostname"] | (const char *)nullptr;
    if (hostname && !validHostname(hostname)) {
        return sendError(req, 400, "Naziv uređaja sme da sadrži samo slova, brojeve i crticu (najviše 31 znak).");
    }
    const char *admin = json["admin_password"] | (const char *)nullptr;
    if (admin && strlen(admin) > 0 && (strlen(admin) < 8 || strlen(admin) > 64)) {
        return sendError(req, 400, "Administratorska lozinka mora imati 8--64 znaka.");
    }

    if (!lock()) return sendError(req, 503, "Uređaj je zauzet, pokušajte ponovo.");
    DeviceConfig &cfg = pendingConfig();
    bool sameNetwork = strcmp(cfg.wifiSsid, ssid) == 0;
    strlcpy(cfg.wifiSsid, ssid, sizeof(cfg.wifiSsid));
    if (pass) strlcpy(cfg.wifiPass, pass, sizeof(cfg.wifiPass));       // izostavljena lozinka = ostaje ista
    else if (!sameNetwork) cfg.wifiPass[0] = '\0';
    cfg.staticIp = staticIp;
    if (staticIp) { cfg.ip = ip; cfg.gateway = gw; cfg.subnet = mask; cfg.dns = dns; }
    if (hostname) strlcpy(cfg.hostname, hostname, sizeof(cfg.hostname));
    if (admin && strlen(admin) > 0) strlcpy(cfg.adminPass, admin, sizeof(cfg.adminPass));
    s_pending |= ACT_WIFI_RECONNECT;
    unlock();
    sendOk(req, "Podešavanja su sačuvana. Uređaj se povezuje na mrežu.");
}

static void handleConfigPost(AsyncWebServerRequest *req, JsonVariant &json) {
    if (!requireAuth(req)) return;

    if (!lock()) return sendError(req, 503, "Uređaj je zauzet, pokušajte ponovo.");
    DeviceConfig cfg = pendingConfig();  // kopija; upisuje se tek kada je sve ispravno
    unlock();

    bool timeChanged = false, wifiChanged = false, displayChanged = false;
    if (json["oled_brightness"].is<int>()) {
        int v = json["oled_brightness"];
        if (v < 0 || v > 100) return sendError(req, 400, "Osvetljenje ekrana mora biti 0--100 %.");
        cfg.oledBrightness = v;
        displayChanged = true;
    }
    if (json["ntp_enabled"].is<bool>()) { cfg.ntpEnabled = json["ntp_enabled"]; timeChanged = true; }
    const char *servers[3] = {json["ntp_server1"] | (const char *)nullptr, json["ntp_server2"] | (const char *)nullptr,
                              json["ntp_server3"] | (const char *)nullptr};
    char *dst[3] = {cfg.ntpServer1, cfg.ntpServer2, cfg.ntpServer3};
    for (int i = 0; i < 3; i++) {
        if (!servers[i]) continue;
        if (strlen(servers[i]) == 0 || strlen(servers[i]) > 63) return sendError(req, 400, "Neispravan NTP server.");
        strlcpy(dst[i], servers[i], 64);
        timeChanged = true;
    }
    if (json["ntp_interval_min"].is<int>()) {
        int v = json["ntp_interval_min"];
        if (v < 15 || v > 1440) return sendError(req, 400, "Interval sinhronizacije mora biti 15--1440 min.");
        cfg.ntpIntervalMin = v;
        timeChanged = true;
    }
    const char *tz = json["timezone"] | (const char *)nullptr;
    if (tz) {
        if (!config_manager::posixTimezone(tz)) return sendError(req, 400, "Nepodržana vremenska zona.");
        strlcpy(cfg.timezone, tz, sizeof(cfg.timezone));
        timeChanged = true;
    }
    const char *hostname = json["hostname"] | (const char *)nullptr;
    if (hostname) {
        if (!validHostname(hostname)) return sendError(req, 400, "Neispravan naziv uređaja.");
        wifiChanged = strcmp(cfg.hostname, hostname) != 0;
        strlcpy(cfg.hostname, hostname, sizeof(cfg.hostname));
    }
    const char *admin = json["admin_password"] | (const char *)nullptr;
    if (admin) {
        if (strlen(admin) < 8 || strlen(admin) > 64) return sendError(req, 400, "Administratorska lozinka mora imati 8--64 znaka.");
        strlcpy(cfg.adminPass, admin, sizeof(cfg.adminPass));
    }
    const char *ap = json["ap_password"] | (const char *)nullptr;
    if (ap) {
        if (strlen(ap) < 8 || strlen(ap) > 63) return sendError(req, 400, "Lozinka pristupne tačke mora imati 8--63 znaka.");
        strlcpy(cfg.apPass, ap, sizeof(cfg.apPass));
    }

    if (!lock()) return sendError(req, 503, "Uređaj je zauzet, pokušajte ponovo.");
    pendingConfig() = cfg;
    if (timeChanged) s_pending |= ACT_TIME_CONFIG;
    if (wifiChanged) s_pending |= ACT_WIFI_RECONNECT;
    if (displayChanged) s_pending |= ACT_DISPLAY;
    unlock();
    sendOk(req, "Podešavanja su sačuvana.");
}

static void handleTimePost(AsyncWebServerRequest *req, JsonVariant &json) {
    if (!requireAuth(req)) return;
    if (!json["epoch"].is<int64_t>()) return sendError(req, 400, "Nedostaje vreme (epoch).");
    int64_t epoch = json["epoch"];
    if (epoch < 1700000000) return sendError(req, 400, "Neispravno vreme.");
    if (!lock()) return sendError(req, 503, "Uređaj je zauzet, pokušajte ponovo.");
    s_pendingEpoch = (time_t)epoch;
    s_pending |= ACT_SET_TIME;
    unlock();
    sendOk(req, "Vreme je podešeno.");
}

static void handleAction(AsyncWebServerRequest *req, Action action, const char *message) {
    if (!requireAuth(req)) return;
    if (!lock()) return sendError(req, 503, "Uređaj je zauzet, pokušajte ponovo.");
    s_pending |= action;
    unlock();
    sendOk(req, message);
}

// ---------------------------------------------------------------------------

static const char FALLBACK_PAGE[] PROGMEM =
    "<!doctype html><html lang=\"sr\"><meta charset=\"utf-8\"><title>Meteo stanica</title>"
    "<body style=\"font-family:sans-serif;padding:2em\"><h1>Meteo stanica</h1>"
    "<p>Web fajlovi nisu učitani u LittleFS particiju.</p>"
    "<p>Pokrenuti: <code>pio run -t uploadfs</code></p>"
    "<p>REST API: <a href=\"/api/v1/status\">/api/v1/status</a></p></body></html>";

void begin() {
    s_mutex = xSemaphoreCreateMutex();

    if (!LittleFS.begin(true)) {
        LOG_E(TAG, "LittleFS nije dostupan");
    } else {
        LOG_I(TAG, "LittleFS: %lu / %lu B zauzeto", (unsigned long)LittleFS.usedBytes(),
              (unsigned long)LittleFS.totalBytes());
    }

    s_server.on("/api/v1/status", HTTP_GET, handleStatus);
    s_server.on("/api/v1/readings", HTTP_GET, handleReadings);
    s_server.on("/api/v1/sensors", HTTP_GET, handleSensors);
    s_server.on("/api/v1/time", HTTP_GET, handleTime);
    s_server.on("/api/v1/wifi/scan", HTTP_GET, handleWifiScan);
    s_server.on("/api/v1/wifi", HTTP_GET, handleWifi);
    s_server.on("/api/v1/config", HTTP_GET, handleConfig);
    s_server.on("/api/v1/system", HTTP_GET, handleSystem);

    s_server.on("/api/v1/wifi", HTTP_POST, handleWifiPost);
    s_server.on("/api/v1/config", HTTP_POST, handleConfigPost);
    s_server.on("/api/v1/time", HTTP_POST, handleTimePost);
    s_server.on("/api/v1/auth", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (requireAuth(req)) sendOk(req, "Prijava uspešna.");
    });
    s_server.on("/api/v1/wifi/portal", HTTP_POST, [](AsyncWebServerRequest *req) {
        handleAction(req, ACT_PORTAL, "Konfiguracioni portal je pokrenut.");
    });
    s_server.on("/api/v1/system/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
        handleAction(req, ACT_RESTART, "Uređaj se restartuje.");
    });

    s_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");

    s_server.onNotFound([](AsyncWebServerRequest *req) {
        // Captive portal: svaki zahtev ka drugom hostu preusmerava se na podešavanje Wi-Fi mreže
        if (wifi_manager::portalActive() && req->host() != WiFi.softAPIP().toString()) {
            req->redirect("http://" + WiFi.softAPIP().toString() + "/#wifi");
            return;
        }
        if (req->url().startsWith("/api/")) {
            sendError(req, 404, "Nepoznata API ruta.");
            return;
        }
        if (req->url() == "/" || req->url() == "/index.html") {
            req->send(200, "text/html; charset=utf-8", FALLBACK_PAGE);
            return;
        }
        req->send(404, "text/plain; charset=utf-8", "Stranica nije pronađena");
    });

    s_server.begin();
    LOG_I(TAG, "Web server pokrenut na portu 80");
}

}  // namespace web_server
