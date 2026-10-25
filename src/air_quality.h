#pragma once

#include <Arduino.h>

#include "metrics.h"

// Nivo kvaliteta vazduha po skali EAQI (specifikacija §10A.1)
// Lokalna procena iz kliznog proseka PM2.5 i PM10; prikazuje se lošiji od dva nivoa.

namespace air_quality {

void update(uint32_t now);

// 0 = nepoznato, 1..6 = Dobar .. Izuzetno loš
uint8_t level();
const char *levelName(uint8_t level);

// Zagađivač koji određuje nivo (PM2.5 ili PM10) i njegov klizni prosek
MetricId dominant();
float dominantAverage();

}  // namespace air_quality
