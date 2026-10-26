#include "logger.h"

#include <stdarg.h>

#include "config.h"

namespace logger {

static Entry s_ring[LOG_RING_SIZE];
static size_t s_head = 0;
static size_t s_count = 0;
static uint32_t s_dropped = 0;
static volatile uint8_t s_level = LOG_DEFAULT_LEVEL;
static SemaphoreHandle_t s_mutex = nullptr;

static const char *const NAMES[] = {"ERROR", "WARN", "INFO", "DEBUG"};

const char *levelName(uint8_t level) { return level <= 3 ? NAMES[level] : "?"; }

void setLevel(uint8_t level) { s_level = level > 3 ? 3 : level; }

uint8_t level() { return s_level; }

void write(uint8_t level, const char *tag, const char *fmt, ...) {
    if (level > s_level) return;

    char msg[sizeof(Entry::msg)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    uint32_t ms = millis();
    Serial.printf("[%10lu] %-5s %s: %s\n", (unsigned long)ms, levelName(level), tag, msg);

    // Mutex se pravi pri prvom pozivu (logovi postoje i pre setup-a ostalih modula)
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_dropped++;
        return;
    }
    Entry &e = s_ring[s_head];
    e.ms = ms;
    e.level = level;
    strlcpy(e.tag, tag, sizeof(e.tag));
    strlcpy(e.msg, msg, sizeof(e.msg));
    s_head = (s_head + 1) % LOG_RING_SIZE;
    if (s_count < LOG_RING_SIZE) s_count++;
    xSemaphoreGive(s_mutex);
}

void forEach(const std::function<void(const Entry &)> &fn) {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    size_t start = (s_head + LOG_RING_SIZE - s_count) % LOG_RING_SIZE;
    for (size_t i = 0; i < s_count; i++) fn(s_ring[(start + i) % LOG_RING_SIZE]);
    xSemaphoreGive(s_mutex);
}

uint32_t droppedCount() { return s_dropped; }

}  // namespace logger
