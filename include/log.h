#pragma once

#include <Arduino.h>

// Nivoi logovanja: 0 = ERROR, 1 = WARN, 2 = INFO, 3 = DEBUG
#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

#define LOG_PRINT(lvl, tag, fmt, ...) \
    Serial.printf("[%10lu] %-5s %s: " fmt "\n", (unsigned long)millis(), lvl, tag, ##__VA_ARGS__)

#define LOG_E(tag, fmt, ...) do { if (LOG_LEVEL >= 0) LOG_PRINT("ERROR", tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_W(tag, fmt, ...) do { if (LOG_LEVEL >= 1) LOG_PRINT("WARN",  tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_I(tag, fmt, ...) do { if (LOG_LEVEL >= 2) LOG_PRINT("INFO",  tag, fmt, ##__VA_ARGS__); } while (0)
#define LOG_D(tag, fmt, ...) do { if (LOG_LEVEL >= 3) LOG_PRINT("DEBUG", tag, fmt, ##__VA_ARGS__); } while (0)
