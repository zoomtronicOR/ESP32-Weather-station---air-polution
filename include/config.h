#pragma once

// Osnovni parametri firmware-a

#define DEVICE_NAME       "esp32_r201"
#define FW_VERSION        "0.6.0"

#define SERIAL_BAUD       115200

// Task watchdog za glavnu petlju
#define WDT_TIMEOUT_MS    8000

// Interval periodičnog ispisa statusa i merenja na serijski port
#define STATUS_INTERVAL_MS  10000
#define METRICS_PRINT_MS    30000

// Ispod ovog praga slobodne heap memorije statusna LED prijavljuje kritičnu grešku
#define LOW_HEAP_BYTES      20000

// I2C magistrale (specifikacija §3.1, §3.2)
#define I2C_FREQUENCY_HZ  100000   // glavna magistrala; 400 kHz nakon dugotrajnog testa
#define I2C1_FREQUENCY_HZ 100000   // SPS30 podržava najviše 100 kHz

// Upravljanje senzorima
#define SENSOR_RETRY_MS     30000  // ponovni pokušaj inicijalizacije nepronađenog senzora
#define SENSOR_MAX_ERRORS   5      // uzastopnih grešaka pre ponovne inicijalizacije

// Intervali merenja (specifikacija §5.1)
#define BME680_INTERVAL_MS  5000   // T, RH, p; gasni otpor na svako drugo merenje (10 s)
#define MPU_INTERVAL_MS     200
#define LTR390_INTERVAL_MS  5000
#define SPS30_INTERVAL_MS   5000
#define SPS30_WARMUP_MS     30000  // posle pokretanja ventilatora merenje još nije pouzdano

// Lokalna istorija (specifikacija §9.2)
#define HISTORY_5MIN_RECORDS   288   // 24 h po 5 min
#define HISTORY_HOUR_RECORDS   720   // 30 dana po 1 h
#define HISTORY_PENDING_MAX    12    // 5-min intervala (1 h) koji čekaju pouzdano vreme

// Kvalitet vazduha (EAQI, specifikacija §10A.1)
#define AQ_WINDOW_MS        600000  // klizni prosek 10 min
#define AQ_HYSTERESIS_MS    60000   // novi nivo mora da traje 60 s
#define AQ_STALE_MS         60000   // bez PM merenja duže od ovoga nivo je nepoznat

// OLED 0,96" 128×64 dvobojni (specifikacija §10)
#define OLED_SPLASH_MS      1500    // uvodni ekran
#define OLED_BOOT_CONN_MS   10000   // podaci za konekciju posle pokretanja
#define OLED_CONN_REPEAT_MS 5000    // ponovni prikaz posle promene veze ili tastera BOOT
#define OLED_PAGE_MS        3000    // trajanje jedne stranice u rotaciji (6 stranica po 3 reda)
#define OLED_REFRESH_MS     1000    // osvežavanje sadržaja
#define OLED_SHIFT_MS       120000  // pomeranje sadržaja za 1 piksel (zaštita od burn-in)
#define OLED_RETRY_MS       30000   // ponovni pokušaj ako ekran ne odgovara

// Vreme (specifikacija §12.9)
#define TIME_STALE_MS       (24UL * 3600UL * 1000UL)  // sinhronizacija starija od 24 h
#define TIME_SYNC_WARN_MS   (15UL * 60UL * 1000UL)    // upozorenje ako nema sinhronizacije 15 min

// Wi-Fi (specifikacija §7)
#define WIFI_AP_SSID            "METEO-ESP32-SETUP"
#define WIFI_CONNECT_TIMEOUT_MS 20000   // jedan pokušaj povezivanja
#define WIFI_MAX_ATTEMPTS       3       // posle ovoliko neuspeha pokreće se portal
#define WIFI_BACKOFF_MIN_MS     5000    // ponovno povezivanje: 5 s, 10 s, 20 s ...
#define WIFI_BACKOFF_MAX_MS     300000  // ... najviše 5 min
#define WIFI_PORTAL_TIMEOUT_MS  300000  // portal se gasi posle 5 min ako postoji sačuvana mreža
#define WIFI_PORTAL_BUTTON_MS   5000    // dug pritisak tastera BOOT ručno pokreće portal

// MQTT (specifikacija §8.5)
#define MQTT_RECONNECT_MS       10000   // pauza između pokušaja povezivanja na broker
#define MQTT_RESEND_MS          60000   // nepromenjene vrednosti se ponovo šalju najkasnije na 60 s
                                        // (HA expire_after je bar 3 × ovaj rok)

// OTA (specifikacija §15)
#define OTA_VERIFY_MS           60000   // novi firmware se potvrđuje posle 60 s stabilnog rada

// Web server i pristup (specifikacija §14)
#define WEB_ADMIN_USER          "admin"
#define AUTH_MAX_FAILURES       5       // neuspešnih prijava pre privremenog zaključavanja
#define AUTH_LOCKOUT_MS         300000  // zaključavanje 5 min

// BME680 grejač gasnog senzora
#define BME680_HEATER_TEMP_C  320
#define BME680_HEATER_MS      150

// MPU: detekcija pomeranja
#define MPU_MOTION_GYRO_DPS   5.0f   // ugaona brzina iznad praga
#define MPU_MOTION_ACCEL_G    0.10f  // odstupanje |a| od 1 g iznad praga
#define MPU_MOTION_HOLD_MS    5000   // koliko dugo se pomeranje prijavljuje posle događaja

// LTR-390: faktor kalibracije UV indeksa (1,0 = vrednost iz datasheet-a, bez kalibracije)
#define LTR390_UVI_CALIBRATION  1.0f
// Faktor prozora/kućišta (WFAC) za lux i UV; 1,0 = bez prozora
#define LTR390_WFAC             1.0f
