#pragma once

#include <ESPAsyncWebServer.h>

// REST rute za istoriju, statistiku i izvoz (specifikacija §5.4, §9.3, §13)
//   GET /api/v1/history?period=1h|6h|24h|7d|30d&metrics=temperature,humidity
//   GET /api/v1/history/export?format=csv|json&period=...
//   GET /api/v1/statistics
// Velike odgovore šalje u delovima (chunked) direktno iz fajla, bez pravljenja celog JSON-a u RAM-u.

namespace history_api {

void registerRoutes(AsyncWebServer &server);

}  // namespace history_api
