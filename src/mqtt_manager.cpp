#include "mqtt_manager.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <mqtt_client.h>

#include "air_quality.h"
#include "config.h"
#include "config_manager.h"
#include "log.h"
#include "metrics.h"
#include "sensor_manager.h"
#include "time_manager.h"

namespace mqtt_manager {

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = nullptr;
static bool s_started = false;

// Događaji iz MQTT zadatka -- obrađuju se u glavnoj petlji
static volatile bool s_connected = false;
static volatile bool s_connectedEvent = false;
static volatile bool s_haOnlineEvent = false;
static volatile int s_lastErrorCode = 0;

static char s_uri[96];
static char s_lwtTopic[80];
static char s_haStatusTopic[48];
static char s_errorText[64] = "";

static uint32_t s_lastPublishMs = 0;
static uint32_t s_lastForceMs = 0;
static uint32_t s_publishCount = 0;
static uint32_t s_connectCount = 0;
static float s_lastSent[(size_t)MetricId::Count];
static bool s_sentValid[(size_t)MetricId::Count];

// ---------------------------------------------------------------------------
// Home Assistant Discovery: opis entiteta po metrici
// ---------------------------------------------------------------------------

struct HaEntity {
    const char *component;     // sensor / binary_sensor
    const char *deviceClass;   // nullptr = bez klase
    const char *unit;          // jedinica za HA (nullptr = bez jedinice)
    const char *icon;          // nullptr = podrazumevana ikona
};

// Redosled prati MetricId. HA koristi grčko μ (U+03BC) u "μg/m³".
static const HaEntity HA_ENTITIES[] = {
    {"sensor", "temperature", "°C", nullptr},                 // Temperature
    {"sensor", "humidity", "%", nullptr},                     // Humidity
    {"sensor", "atmospheric_pressure", "hPa", nullptr},       // Pressure
    {"sensor", nullptr, "Ω", "mdi:gas-cylinder"},             // GasResistance
    {"sensor", "pm1", "μg/m³", nullptr},                      // Pm1_0
    {"sensor", "pm25", "μg/m³", nullptr},                     // Pm2_5
    {"sensor", nullptr, "μg/m³", "mdi:blur"},                 // Pm4 (HA nema klasu za PM4)
    {"sensor", "pm10", "μg/m³", nullptr},                     // Pm10
    {"sensor", nullptr, "µm", "mdi:dots-hexagon"},            // ParticleSize
    {"sensor", nullptr, nullptr, "mdi:weather-sunny-alert"},  // UvIndex
    {"sensor", "illuminance", "lx", nullptr},                 // Illuminance
    {"sensor", nullptr, "°", "mdi:angle-acute"},              // Pitch
    {"sensor", nullptr, "°", "mdi:angle-acute"},              // Roll
    {"binary_sensor", "moving", nullptr, nullptr},            // Motion
    {"sensor", nullptr, nullptr, "mdi:air-filter"},           // AirQualityLevel (EAQI 1–6)
};
static_assert(sizeof(HA_ENTITIES) / sizeof(HA_ENTITIES[0]) == (size_t)MetricId::Count,
              "HA_ENTITIES mora pratiti MetricId");

static void deviceId(char *buf, size_t len) {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(buf, len, "%s_%02x%02x%02x", DEVICE_NAME, (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
}

static void addDevice(JsonObject dev) {
    char id[40];
    deviceId(id, sizeof(id));
    dev["identifiers"].to<JsonArray>().add(id);
    dev["name"] = "Meteo stanica";
    dev["model"] = "ESP32 R201 meteo stanica";
    dev["manufacturer"] = "DIY";
    dev["sw_version"] = FW_VERSION;
    if (WiFi.isConnected()) {
        dev["configuration_url"] = "http://" + WiFi.localIP().toString() + "/";
    }
}

static bool publish(const char *topic, const char *payload, int qos, bool retain) {
    if (!s_client || !s_connected) return false;
    // enqueue ne blokira glavnu petlju; poruku šalje MQTT zadatak
    int id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain ? 1 : 0, true);
    if (id < 0) {
        LOG_W(TAG, "Poruka nije stavljena u red: %s", topic);
        return false;
    }
    return true;
}

static void publishDiscoveryEntity(const char *component, const char *objectId, const char *name,
                                   const char *stateTopic, const char *deviceClass, const char *unit,
                                   const char *icon, const char *stateClass, bool diagnostic, int precision,
                                   uint32_t expireAfterS, const char *payloadOn = nullptr,
                                   const char *payloadOff = nullptr) {
    const DeviceConfig &cfg = config_manager::get();
    char id[40], uniqueId[80], topic[160];
    deviceId(id, sizeof(id));
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", id, objectId);
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", cfg.mqttDiscoveryPrefix, component, id, objectId);

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = uniqueId;
    doc["state_topic"] = stateTopic;
    doc["availability_topic"] = s_lwtTopic;
    if (deviceClass) doc["device_class"] = deviceClass;
    if (unit) doc["unit_of_measurement"] = unit;
    if (icon) doc["icon"] = icon;
    if (stateClass) doc["state_class"] = stateClass;
    if (diagnostic) doc["entity_category"] = "diagnostic";
    if (precision >= 0) doc["suggested_display_precision"] = precision;
    if (expireAfterS) doc["expire_after"] = expireAfterS;
    if (payloadOn) doc["payload_on"] = payloadOn;
    if (payloadOff) doc["payload_off"] = payloadOff;
    addDevice(doc["device"].to<JsonObject>());

    String payload;
    serializeJson(doc, payload);
    publish(topic, payload.c_str(), 1, true);  // retained (specifikacija §8.4)
}

static void publishDiscovery() {
    const DeviceConfig &cfg = config_manager::get();
    // Merenje koje se ne osveži za 3 intervala HA prikazuje kao nedostupno
    uint32_t expire = (uint32_t)cfg.mqttIntervalS * 3 + 30;
    char stateTopic[128];

    for (size_t i = 0; i < (size_t)MetricId::Count; i++) {
        const MetricDef &d = metrics::def((MetricId)i);
        const HaEntity &e = HA_ENTITIES[i];
        snprintf(stateTopic, sizeof(stateTopic), "%s/%s", cfg.mqttBaseTopic, d.key);
        bool binary = strcmp(e.component, "binary_sensor") == 0;
        publishDiscoveryEntity(e.component, d.key, d.name, stateTopic, e.deviceClass, e.unit, e.icon,
                               binary ? nullptr : "measurement", false, binary ? -1 : d.decimals, expire,
                               binary ? "1" : nullptr, binary ? "0" : nullptr);
    }

    // Dijagnostika uređaja
    snprintf(stateTopic, sizeof(stateTopic), "%s/wifi_rssi", cfg.mqttBaseTopic);
    publishDiscoveryEntity("sensor", "wifi_rssi", "Wi-Fi signal", stateTopic, "signal_strength", "dBm", nullptr,
                           "measurement", true, 0, expire);
    snprintf(stateTopic, sizeof(stateTopic), "%s/uptime", cfg.mqttBaseTopic);
    publishDiscoveryEntity("sensor", "uptime", "Vreme rada", stateTopic, "duration", "s", nullptr,
                           "total_increasing", true, 0, expire);
    snprintf(stateTopic, sizeof(stateTopic), "%s/sensors_ok", cfg.mqttBaseTopic);
    publishDiscoveryEntity("sensor", "sensors_ok", "Ispravni senzori", stateTopic, nullptr, nullptr,
                           "mdi:check-network", "measurement", true, 0, expire);
    snprintf(stateTopic, sizeof(stateTopic), "%s/time_last_sync", cfg.mqttBaseTopic);
    publishDiscoveryEntity("sensor", "time_last_sync", "Poslednja NTP sinhronizacija", stateTopic, "timestamp",
                           nullptr, nullptr, nullptr, true, -1, 0);
    snprintf(stateTopic, sizeof(stateTopic), "%s/time_status", cfg.mqttBaseTopic);
    publishDiscoveryEntity("sensor", "time_status", "Status vremena", stateTopic, nullptr, nullptr,
                           "mdi:clock-check-outline", nullptr, true, -1, 0);
    // Status MQTT veze: binarni senzor nad availability topic-om
    publishDiscoveryEntity("binary_sensor", "mqtt_connected", "MQTT veza", s_lwtTopic, "connectivity", nullptr,
                           nullptr, nullptr, true, -1, 0, "online", "offline");

    LOG_I(TAG, "Home Assistant Discovery objavljen (prefiks \"%s\")", cfg.mqttDiscoveryPrefix);
}

// ---------------------------------------------------------------------------
// Objavljivanje merenja
// ---------------------------------------------------------------------------

static void publishReadings(uint32_t now, bool force) {
    const DeviceConfig &cfg = config_manager::get();
    char topic[128], value[32];
    int qos = cfg.mqttQos;

    JsonDocument doc;
    doc["device"] = DEVICE_NAME;
    if (time_manager::isValid()) {
        char ts[24];
        time_manager::formatIsoUtc(time_manager::nowUtc(), ts, sizeof(ts));
        doc["timestamp"] = ts;
    } else {
        doc["timestamp"] = nullptr;
        doc["time_valid"] = false;  // specifikacija §12.8
    }

    for (size_t i = 0; i < (size_t)MetricId::Count; i++) {
        MetricId id = (MetricId)i;
        const MetricDef &d = metrics::def(id);
        if (!metrics::isFresh(id, now)) {
            doc[d.key] = nullptr;  // nevalidna vrednost se ne šalje kao broj (specifikacija §5.3)
            s_sentValid[i] = false;
            continue;
        }
        float v = metrics::get(id).value;
        snprintf(value, sizeof(value), "%.*f", d.decimals, v);
        doc[d.key] = serialized(String(value));

        // Ista vrednost se ne šalje nepotrebno često (specifikacija §8.5)
        float step = d.decimals == 0 ? 1.0f : powf(10.0f, -d.decimals);
        bool changed = !s_sentValid[i] || fabsf(v - s_lastSent[i]) >= step * 0.5f;
        if (changed || force) {
            snprintf(topic, sizeof(topic), "%s/%s", cfg.mqttBaseTopic, d.key);
            if (publish(topic, value, qos, false)) {
                s_lastSent[i] = v;
                s_sentValid[i] = true;
            }
        }
    }

    // Objedinjeni JSON paket (specifikacija §8.3)
    String json;
    serializeJson(doc, json);
    snprintf(topic, sizeof(topic), "%s/state", cfg.mqttBaseTopic);
    publish(topic, json.c_str(), qos, false);

    // Dijagnostika
    snprintf(topic, sizeof(topic), "%s/wifi_rssi", cfg.mqttBaseTopic);
    snprintf(value, sizeof(value), "%d", WiFi.RSSI());
    publish(topic, value, qos, false);
    snprintf(topic, sizeof(topic), "%s/uptime", cfg.mqttBaseTopic);
    snprintf(value, sizeof(value), "%lu", (unsigned long)(now / 1000));
    publish(topic, value, qos, false);
    sensor_manager::Health h = sensor_manager::health();
    snprintf(topic, sizeof(topic), "%s/sensors_ok", cfg.mqttBaseTopic);
    snprintf(value, sizeof(value), "%u", h.ok);
    publish(topic, value, qos, false);

    JsonDocument st;
    st["sensors_ok"] = h.ok;
    st["sensors_total"] = h.total;
    st["air_quality"] = air_quality::levelName(air_quality::level());
    st["time"] = time_manager::statusKey(time_manager::status());
    st["firmware"] = FW_VERSION;
    String stJson;
    serializeJson(st, stJson);
    snprintf(topic, sizeof(topic), "%s/status", cfg.mqttBaseTopic);
    publish(topic, stJson.c_str(), qos, true);

    snprintf(topic, sizeof(topic), "%s/time_status", cfg.mqttBaseTopic);
    publish(topic, time_manager::statusKey(time_manager::status()), qos, true);
    if (time_manager::lastSyncUtc() > 0) {
        char ts[24];
        time_manager::formatIsoUtc(time_manager::lastSyncUtc(), ts, sizeof(ts));
        snprintf(topic, sizeof(topic), "%s/time_last_sync", cfg.mqttBaseTopic);
        publish(topic, ts, qos, true);
    }

    s_publishCount++;
    s_lastPublishMs = now;
}

// ---------------------------------------------------------------------------
// Klijent
// ---------------------------------------------------------------------------

static void onMqttEvent(void *, esp_event_base_t, int32_t eventId, void *eventData) {
    // Izvršava se u MQTT zadatku -- samo postavljanje indikatora
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)eventData;
    switch ((esp_mqtt_event_id_t)eventId) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_connectedEvent = true;
            s_lastErrorCode = 0;
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            break;
        case MQTT_EVENT_DATA:
            // Home Assistant je ponovo pokrenut -- ponovo objaviti Discovery
            if (ev->topic_len == (int)strlen(s_haStatusTopic) && strncmp(ev->topic, s_haStatusTopic, ev->topic_len) == 0 &&
                ev->data_len == 6 && strncmp(ev->data, "online", 6) == 0) {
                s_haOnlineEvent = true;
            }
            break;
        case MQTT_EVENT_ERROR:
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                s_lastErrorCode = 100 + ev->error_handle->connect_return_code;
            } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                s_lastErrorCode = 1;
            }
            break;
        default:
            break;
    }
}

static void stopClient() {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    s_started = false;
    s_connected = false;
}

static void startClient() {
    const DeviceConfig &cfg = config_manager::get();
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u", cfg.mqttHost, cfg.mqttPort);
    snprintf(s_lwtTopic, sizeof(s_lwtTopic), "%s/availability", cfg.mqttBaseTopic);
    snprintf(s_haStatusTopic, sizeof(s_haStatusTopic), "%s/status", cfg.mqttDiscoveryPrefix);

    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri = s_uri;
    mc.credentials.client_id = cfg.mqttClientId;
    if (cfg.mqttUser[0]) {
        mc.credentials.username = cfg.mqttUser;
        mc.credentials.authentication.password = cfg.mqttPass;
    }
    // Last Will and Testament: broker objavljuje "offline" ako uređaj nestane (specifikacija §8.4)
    mc.session.last_will.topic = s_lwtTopic;
    mc.session.last_will.msg = "offline";
    mc.session.last_will.msg_len = 7;
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
    mc.session.keepalive = 30;
    mc.network.reconnect_timeout_ms = MQTT_RECONNECT_MS;
    mc.network.timeout_ms = 5000;
    mc.buffer.size = 2048;
    mc.buffer.out_size = 2048;

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        LOG_E(TAG, "Inicijalizacija MQTT klijenta nije uspela");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, onMqttEvent, nullptr);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        LOG_E(TAG, "Pokretanje MQTT klijenta nije uspelo");
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return;
    }
    s_started = true;
    // Lozinka se ne upisuje u log (specifikacija §8.1)
    LOG_I(TAG, "Povezivanje na broker %s (client id \"%s\")", s_uri, cfg.mqttClientId);
}

void begin() {}

void restart() {
    stopClient();
    for (size_t i = 0; i < (size_t)MetricId::Count; i++) s_sentValid[i] = false;
    s_errorText[0] = '\0';
}

void update(uint32_t now, bool networkUp) {
    const DeviceConfig &cfg = config_manager::get();
    bool shouldRun = cfg.mqttEnabled && cfg.mqttHost[0] != '\0';

    if (!shouldRun) {
        if (s_started) {
            stopClient();
            LOG_I(TAG, "MQTT isključen");
        }
        return;
    }
    if (!s_started && networkUp) {
        startClient();
    }
    if (!s_client) return;

    if (s_connectedEvent) {
        s_connectedEvent = false;
        s_connectCount++;
        LOG_I(TAG, "Povezan na broker %s", s_uri);
        publish(s_lwtTopic, "online", 1, true);
        if (cfg.mqttDiscovery) {
            esp_mqtt_client_subscribe(s_client, s_haStatusTopic, 0);
            publishDiscovery();
        }
        for (size_t i = 0; i < (size_t)MetricId::Count; i++) s_sentValid[i] = false;
        s_lastPublishMs = 0;  // odmah objaviti merenja
    }
    if (s_haOnlineEvent) {
        s_haOnlineEvent = false;
        if (cfg.mqttDiscovery) {
            LOG_I(TAG, "Home Assistant ponovo pokrenut -- ponovo se objavljuje Discovery");
            publishDiscovery();
        }
    }

    int err = s_lastErrorCode;
    if (err == 0) s_errorText[0] = '\0';
    else if (err == 1) strlcpy(s_errorText, "broker nije dostupan", sizeof(s_errorText));
    else if (err == 100 + MQTT_CONNECTION_REFUSE_BAD_USERNAME || err == 100 + MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)
        strlcpy(s_errorText, "pogrešno korisničko ime ili lozinka", sizeof(s_errorText));
    else if (err == 100 + MQTT_CONNECTION_REFUSE_ID_REJECTED)
        strlcpy(s_errorText, "broker odbija Client ID", sizeof(s_errorText));
    else snprintf(s_errorText, sizeof(s_errorText), "broker odbija vezu (kod %d)", err - 100);

    if (s_connected && (s_lastPublishMs == 0 || now - s_lastPublishMs >= (uint32_t)cfg.mqttIntervalS * 1000)) {
        bool force = now - s_lastForceMs >= MQTT_FORCE_MS;
        if (force) s_lastForceMs = now;
        publishReadings(now, force);
    }
}

bool enabled() { return config_manager::get().mqttEnabled; }

bool connected() { return s_connected; }

const char *lastError() { return s_errorText; }

uint32_t publishCount() { return s_publishCount; }

uint32_t lastPublishAgeS() { return s_lastPublishMs ? (millis() - s_lastPublishMs) / 1000 : 0; }

uint32_t connectCount() { return s_connectCount; }

}  // namespace mqtt_manager
