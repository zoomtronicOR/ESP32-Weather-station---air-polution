#include "diagnostics.h"

#include <Preferences.h>

#include "config.h"
#include "log.h"

namespace diagnostics {

static const char *TAG = "diag";

static uint32_t s_bootCount = 0;
static esp_reset_reason_t s_resetReason = ESP_RST_UNKNOWN;

void begin() {
    s_resetReason = esp_reset_reason();

    // Jedan upis u NVS po pokretanju
    Preferences prefs;
    if (prefs.begin("diag", false)) {
        s_bootCount = prefs.getUInt("boot_cnt", 0) + 1;
        prefs.putUInt("boot_cnt", s_bootCount);
        prefs.end();
    } else {
        LOG_E(TAG, "NVS nije dostupan, brojač pokretanja se ne čuva");
    }

    LOG_I(TAG, "Pokretanje #%lu, razlog restarta: %s",
          (unsigned long)s_bootCount, resetReasonText(s_resetReason));

    if (s_resetReason == ESP_RST_BROWNOUT) {
        LOG_W(TAG, "Restart zbog pada napona -- proveriti USB kabl i napajanje");
    } else if (s_resetReason == ESP_RST_TASK_WDT || s_resetReason == ESP_RST_INT_WDT ||
               s_resetReason == ESP_RST_WDT) {
        LOG_W(TAG, "Restart izazvao watchdog");
    } else if (s_resetReason == ESP_RST_PANIC) {
        LOG_W(TAG, "Restart zbog greške u firmware-u (panic)");
    }
}

void printChipInfo() {
    uint64_t mac = ESP.getEfuseMac();

    Serial.println();
    Serial.println("================ Podaci o uređaju ================");
    Serial.printf("Uređaj:            %s\n", DEVICE_NAME);
    Serial.printf("Verzija firmware-a: %s\n", FW_VERSION);
    // getChipRevision() vraća major * 100 + minor
    uint16_t rev = ESP.getChipRevision();
    Serial.printf("Čip:               %s, revizija v%u.%u\n", ESP.getChipModel(), rev / 100, rev % 100);
    Serial.printf("Broj jezgara:      %u\n", ESP.getChipCores());
    Serial.printf("Frekvencija CPU:   %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
    Serial.printf("Flash:             %lu MB, %lu MHz\n",
                  (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
                  (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
    Serial.printf("PSRAM:             %lu B\n", (unsigned long)ESP.getPsramSize());
    Serial.printf("MAC adresa:        %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac), (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
                  (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    Serial.printf("ESP-IDF:           %s\n", ESP.getSdkVersion());
    Serial.printf("Arduino-ESP32:     %s\n", ESP_ARDUINO_VERSION_STR);
    Serial.printf("Firmware:          %lu B (veličina OTA particije: %lu B)\n",
                  (unsigned long)ESP.getSketchSize(), (unsigned long)ESP.getFreeSketchSpace());
    Serial.printf("Heap:              %lu B ukupno, %lu B slobodno\n",
                  (unsigned long)ESP.getHeapSize(), (unsigned long)ESP.getFreeHeap());
    Serial.printf("Broj pokretanja:   %lu\n", (unsigned long)s_bootCount);
    Serial.printf("Razlog restarta:   %s\n", resetReasonText(s_resetReason));
    Serial.println("==================================================");
    Serial.println();
}

void printStatus() {
    uint32_t sec = millis() / 1000;
    LOG_I(TAG, "Uptime %luh %02lum %02lus | heap slobodno %lu B, min %lu B, najveći blok %lu B",
          (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60),
          (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
          (unsigned long)ESP.getMaxAllocHeap());
}

uint32_t bootCount() { return s_bootCount; }

esp_reset_reason_t resetReason() { return s_resetReason; }

const char *resetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "uključenje napajanja";
        case ESP_RST_EXT:       return "spoljašnji reset";
        case ESP_RST_SW:        return "softverski restart";
        case ESP_RST_PANIC:     return "greška (panic)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "ostali watchdog";
        case ESP_RST_DEEPSLEEP: return "buđenje iz deep sleep-a";
        case ESP_RST_BROWNOUT:  return "pad napona (brownout)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "nepoznat";
    }
}

}  // namespace diagnostics
