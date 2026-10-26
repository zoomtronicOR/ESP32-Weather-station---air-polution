#pragma once

#include <Arduino.h>

// Nivoi logovanja: 0 = ERROR, 1 = WARN, 2 = INFO, 3 = DEBUG (specifikacija §16)
// LOG_LEVEL je najviši nivo koji se uopšte prevodi; nivo u radu se menja preko logger::setLevel().
#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

namespace logger {
void write(uint8_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
}

#define LOG_E(tag, fmt, ...) do { if (LOG_LEVEL >= 0) logger::write(0, tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_W(tag, fmt, ...) do { if (LOG_LEVEL >= 1) logger::write(1, tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_I(tag, fmt, ...) do { if (LOG_LEVEL >= 2) logger::write(2, tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_D(tag, fmt, ...) do { if (LOG_LEVEL >= 3) logger::write(3, tag, fmt, ##__VA_ARGS__); } while (0)
