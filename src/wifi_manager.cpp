#include "wifi_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "config_manager.h"
#include "log.h"

namespace wifi_manager {

static const char *TAG = "wifi";

enum class StaState : uint8_t { NoCredentials, Connecting, Connected, WaitRetry };

static StaState s_state = StaState::NoCredentials;
static uint32_t s_stateStartMs = 0;
static uint32_t s_backoffMs = WIFI_BACKOFF_MIN_MS;
static uint8_t s_attempts = 0;
static uint32_t s_reconnects = 0;
static uint32_t s_connectedSinceMs = 0;

static bool s_portal = false;
static uint32_t s_portalStartMs = 0;
static uint32_t s_portalCloseAtMs = 0;  // posle uspešnog povezivanja portal se gasi uz kratak odlog
static DNSServer s_dns;

bool hasCredentials() { return config_manager::get().wifiSsid[0] != '\0'; }

static void startConnect(uint32_t now) {
    const DeviceConfig &cfg = config_manager::get();
    if (cfg.staticIp) {
        WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.subnet), IPAddress(cfg.dns));
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);  // DHCP
    }
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    s_state = StaState::Connecting;
    s_stateStartMs = now;
    LOG_I(TAG, "Povezivanje na mrežu \"%s\" (pokušaj %u)", cfg.wifiSsid, s_attempts + 1);
}

static void stopPortal() {
    if (!s_portal) return;
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_portal = false;
    s_portalCloseAtMs = 0;
    LOG_I(TAG, "Konfiguracioni portal ugašen");
}

void startPortal(uint32_t now) {
    if (s_portal) {
        s_portalStartMs = now;  // produžava trajanje
        return;
    }
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID, config_manager::apPassword());
    s_dns.start(53, "*", WiFi.softAPIP());
    s_portal = true;
    s_portalStartMs = now;
    s_portalCloseAtMs = 0;
    // Lozinka se ne upisuje u log (specifikacija §14); prikazuje se samo na OLED-u
    LOG_I(TAG, "Konfiguracioni portal: mreža \"%s\", adresa %s", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void onConnected(uint32_t now) {
    s_state = StaState::Connected;
    s_attempts = 0;
    s_backoffMs = WIFI_BACKOFF_MIN_MS;
    s_connectedSinceMs = now;
    LOG_I(TAG, "Povezan na \"%s\", IP %s, signal %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
          WiFi.RSSI());

    MDNS.end();
    if (MDNS.begin(config_manager::get().hostname)) {
        MDNS.addService("http", "tcp", 80);
        LOG_I(TAG, "mDNS: http://%s.local", config_manager::get().hostname);
    }

    if (s_portal) {
        // Korisnik na portalu treba da vidi novu IP adresu pre gašenja pristupne tačke
        s_portalCloseAtMs = now + 60000;
    }
}

void begin(uint32_t now) {
    WiFi.persistent(false);  // podešavanja čuva config_manager, ne Wi-Fi drajver
    WiFi.setHostname(config_manager::get().hostname);

    if (hasCredentials()) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);  // ponovnim povezivanjem upravlja ovaj modul
        startConnect(now);
    } else {
        // Bez sačuvane mreže drajver se odmah pokreće u AP+STA režimu. Brz prelazak
        // STA -> AP+STA odmah posle pokretanja izazivao je reset (interrupt watchdog).
        LOG_W(TAG, "Wi-Fi mreža nije podešena");
        s_state = StaState::NoCredentials;
        startPortal(now);
        WiFi.setAutoReconnect(false);
    }
}

void update(uint32_t now) {
    if (s_portal) {
        s_dns.processNextRequest();
    }

    switch (s_state) {
        case StaState::NoCredentials:
            break;

        case StaState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                onConnected(now);
            } else if (now - s_stateStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
                s_attempts++;
                WiFi.disconnect();
                LOG_W(TAG, "Povezivanje nije uspelo, sledeći pokušaj za %lu s", (unsigned long)(s_backoffMs / 1000));
                if (s_attempts >= WIFI_MAX_ATTEMPTS && !s_portal) {
                    startPortal(now);
                }
                s_state = StaState::WaitRetry;
                s_stateStartMs = now;
            }
            break;

        case StaState::Connected:
            if (WiFi.status() != WL_CONNECTED) {
                LOG_W(TAG, "Veza sa Wi-Fi mrežom je izgubljena");
                s_connectedSinceMs = 0;
                s_reconnects++;
                s_backoffMs = WIFI_BACKOFF_MIN_MS;
                s_state = StaState::WaitRetry;
                s_stateStartMs = now;
            }
            break;

        case StaState::WaitRetry:
            if (now - s_stateStartMs >= s_backoffMs) {
                s_backoffMs = min<uint32_t>(s_backoffMs * 2, WIFI_BACKOFF_MAX_MS);
                startConnect(now);
            }
            break;
    }

    if (s_portal) {
        bool closeAfterConnect = s_portalCloseAtMs != 0 && (int32_t)(now - s_portalCloseAtMs) >= 0;
        // Pristupna tačka ne sme ostati trajno otvorena kada postoji sačuvana mreža (specifikacija §7.3)
        bool timedOut = hasCredentials() && s_state != StaState::Connected && WiFi.softAPgetStationNum() == 0 &&
                        now - s_portalStartMs >= WIFI_PORTAL_TIMEOUT_MS;
        if (closeAfterConnect || timedOut) {
            stopPortal();
        }
    }
}

void reconnect(uint32_t now) {
    WiFi.disconnect();
    WiFi.setHostname(config_manager::get().hostname);
    s_attempts = 0;
    s_backoffMs = WIFI_BACKOFF_MIN_MS;
    s_connectedSinceMs = 0;
    if (hasCredentials()) {
        startConnect(now);
    } else {
        s_state = StaState::NoCredentials;
        startPortal(now);
    }
}

bool connected() { return s_state == StaState::Connected; }

bool portalActive() { return s_portal; }

ConnectionInfo connectionInfo() {
    ConnectionInfo info;
    const DeviceConfig &cfg = config_manager::get();
    if (s_portal) {
        info.mode = NetMode::Portal;
    } else if (s_state == StaState::Connected) {
        info.mode = NetMode::Connected;
    } else if (s_state == StaState::Connecting || s_state == StaState::WaitRetry) {
        info.mode = NetMode::Connecting;
    } else {
        info.mode = NetMode::Off;
    }
    strlcpy(info.ssid, cfg.wifiSsid, sizeof(info.ssid));
    if (s_state == StaState::Connected) {
        strlcpy(info.ip, WiFi.localIP().toString().c_str(), sizeof(info.ip));
        info.rssi = WiFi.RSSI();
    }
    strlcpy(info.apSsid, WIFI_AP_SSID, sizeof(info.apSsid));
    strlcpy(info.apPassword, config_manager::apPassword(), sizeof(info.apPassword));
    strlcpy(info.apIp, WiFi.softAPIP().toString().c_str(), sizeof(info.apIp));
    return info;
}

uint32_t reconnectCount() { return s_reconnects; }

uint32_t connectedSinceMs() { return s_connectedSinceMs; }

}  // namespace wifi_manager
