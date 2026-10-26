#pragma once

#include <Arduino.h>
#include <functional>

// Alarmi: korisnički pragovi sa histerezom i minimalnim trajanjem, aktivni alarmi i događaji
// (specifikacija §11). Pragovi su korisnički; gde postoji zvanična skala, navedena je kao referenca.

namespace alarm_manager {

enum class Kind : uint8_t { Above, Below, Change3h, Condition };
enum class State : uint8_t { Inactive, Pending, Active };
enum class EventType : uint8_t { Activated = 1, Resolved = 2, Acknowledged = 3 };

struct RuleInfo {
    const char *id;
    const char *name;
    Kind kind;
    const char *unit;
    uint8_t decimals;
    const char *reference;  // izvor praga ili "korisnički prag"
};

struct RuleConfig {
    bool enabled;
    float threshold;
    float hysteresis;
    uint16_t minDurationS;
};

struct RuleStatus {
    State state;
    bool acknowledged;
    uint32_t sinceMs;       // početak prekoračenja (uptime)
    uint32_t activatedUtc;  // 0 ako vreme nije bilo pouzdano
    uint32_t activatedMs;
    float value;            // poslednja vrednost
    float peak;             // najgora vrednost tokom aktivnog alarma
    bool hasValue;
};

struct __attribute__((packed)) Event {
    uint32_t utc;     // 0 ako vreme nije bilo pouzdano
    uint32_t uptimeMs;
    uint8_t rule;
    uint8_t type;     // EventType
    float value;
};

void begin();
void update(uint32_t now);

size_t ruleCount();
const RuleInfo &info(size_t i);
const RuleConfig &config(size_t i);
const RuleStatus &status(size_t i);
int find(const char *id);  // -1 ako ne postoji
size_t activeCount();

// Pozivaju se iz glavne petlje
void setConfig(size_t i, const RuleConfig &cfg);
void saveConfig();
void acknowledge(int i);  // -1 = svi aktivni
void clearEvents();

// Događaji od najnovijeg ka najstarijem (čita fajl; bezbedno iz web zadatka)
void forEachEvent(size_t max, const std::function<void(const Event &)> &fn);

// Za MQTT: brojač promena stanja i red novih događaja
uint32_t changeCounter();
bool popEvent(Event &out);

const char *stateKey(State s);  // inactive / pending / active
const char *kindKey(Kind k);    // above / below / change3h / condition

}  // namespace alarm_manager
