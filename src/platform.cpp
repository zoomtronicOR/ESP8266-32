#include "platform.h"

#include <LittleFS.h>
#include <time.h>

#if defined(ESP8266)
#include <user_interface.h>
#elif defined(ESP32)
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_wifi.h>
#endif

namespace Platform {

#if defined(ESP8266)
// =================================================================== ESP8266

uint32_t chipId() { return ESP.getChipId(); }
uint32_t flashSize() { return ESP.getFlashChipRealSize(); }
String coreVersion() { return ESP.getCoreVersion(); }
uint32_t heapTotal() { return 81920; }  // DRAM: data + bss + heap
uint8_t heapFragmentation() { return ESP.getHeapFragmentation(); }
uint32_t maxFreeBlock() { return ESP.getMaxFreeBlockSize(); }
String resetReason() { return ESP.getResetReason(); }

bool resetByWatchdog() {
    uint32_t r = ESP.getResetInfoPtr()->reason;
    return r == REASON_WDT_RST || r == REASON_SOFT_WDT_RST;
}
bool resetByCrash() { return ESP.getResetInfoPtr()->reason == REASON_EXCEPTION_RST; }
uint32_t crashCause() { return ESP.getResetInfoPtr()->exccause; }
void eraseWifiConfig() { ESP.eraseConfig(); }

// LittleFS on ESP8266 auto-formats an unformatted partition on first mount.
bool fsBegin() { return LittleFS.begin(); }
size_t fsTotal() {
    FSInfo i;
    LittleFS.info(i);
    return i.totalBytes;
}
size_t fsUsed() {
    FSInfo i;
    LittleFS.info(i);
    return i.usedBytes;
}
std::vector<String> listDir(const char* dir) {
    std::vector<String> names;
    Dir d = LittleFS.openDir(dir);
    while (d.next()) names.push_back(d.fileName());
    return names;
}

String macSuffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char s[5];
    snprintf(s, sizeof(s), "%02X%02X", mac[4], mac[5]);
    return s;
}

void wifiPrepare(const char* hostname) {
    WiFi.setSleepMode(WIFI_NONE_SLEEP);  // USB powered; keeps web latency low
    WiFi.hostname(hostname);
}
void startScan(uint8_t channel) { WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true, channel); }
bool scanHidden(int i) { return WiFi.isHidden(i); }
bool scanReportsWidth() { return false; }
void scanWidth(int, uint8_t& width, int8_t& secondary) {  // not in the ESP8266 scan results
    width = 0;
    secondary = 0;
}
bool encIsOpen(uint8_t enc) { return enc == ENC_TYPE_NONE; }

// The ESP8266 scan cannot tell WPA3 apart; CCMP is reported as "WPA2".
const char* securityName(uint8_t enc) {
    switch (enc) {
        case ENC_TYPE_NONE: return "Open";
        case ENC_TYPE_WEP:  return "WEP";
        case ENC_TYPE_TKIP: return "WPA";
        case ENC_TYPE_CCMP: return "WPA2";
        case ENC_TYPE_AUTO: return "WPA/WPA2";
        default:            return "Unknown";
    }
}
bool staWrongPassword() { return WiFi.status() == WL_WRONG_PASSWORD; }
void startTime(const char* tz, const char* server) { configTime(tz, server); }
void mdnsUpdate() { MDNS.update(); }

#elif defined(ESP32)
// ===================================================================== ESP32

uint32_t chipId() {
    uint64_t mac = ESP.getEfuseMac();
    return (uint32_t)(mac >> 24);  // low 3 bytes of the MAC, like the ESP8266 chip id
}
uint32_t flashSize() { return ESP.getFlashChipSize(); }
String coreVersion() { return ESP_ARDUINO_VERSION_STR; }
uint32_t heapTotal() { return ESP.getHeapSize(); }
uint8_t heapFragmentation() {
    uint32_t freeHeap = ESP.getFreeHeap();
    return freeHeap ? (uint8_t)(100 - (uint64_t)ESP.getMaxAllocHeap() * 100 / freeHeap) : 0;
}
uint32_t maxFreeBlock() { return ESP.getMaxAllocHeap(); }

String resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return F("Power on");
        case ESP_RST_EXT:       return F("External reset");
        case ESP_RST_SW:        return F("Software restart");
        case ESP_RST_PANIC:     return F("Crash (exception)");
        case ESP_RST_INT_WDT:   return F("Interrupt watchdog");
        case ESP_RST_TASK_WDT:  return F("Task watchdog");
        case ESP_RST_WDT:       return F("Watchdog");
        case ESP_RST_BROWNOUT:  return F("Brownout (power supply)");
        case ESP_RST_USB:       return F("USB reset");
        case ESP_RST_JTAG:      return F("JTAG reset");
        case ESP_RST_DEEPSLEEP: return F("Deep sleep wake-up");
        default:                return F("Unknown");
    }
}
bool resetByWatchdog() {
    esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}
bool resetByCrash() { return esp_reset_reason() == ESP_RST_PANIC; }
uint32_t crashCause() { return 0; }  // not exposed by the ESP32 core
void eraseWifiConfig() { esp_wifi_restore(); }

bool fsBegin() { return LittleFS.begin(/*formatOnFail=*/true); }
size_t fsTotal() { return LittleFS.totalBytes(); }
size_t fsUsed() { return LittleFS.usedBytes(); }
std::vector<String> listDir(const char* dir) {
    std::vector<String> names;
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return names;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String n = f.name();
        int slash = n.lastIndexOf('/');
        names.push_back(slash >= 0 ? n.substring(slash + 1) : n);
    }
    return names;
}

// WiFi.macAddress() reads zeros before the WiFi driver starts; the eFuse MAC is always there.
String macSuffix() {
    uint64_t m = ESP.getEfuseMac();  // byte 0 = first MAC byte
    char s[5];
    snprintf(s, sizeof(s), "%02X%02X", (unsigned)((m >> 32) & 0xFF), (unsigned)((m >> 40) & 0xFF));
    return s;
}

void wifiPrepare(const char* hostname) {
    WiFi.setHostname(hostname);
    WiFi.setSleep(false);  // USB powered; keeps web latency low (BLE coexistence handles the radio)
}
void startScan(uint8_t channel) {
    // 120 ms per channel (core default 300) keeps a full scan around 2 s, like the ESP8266
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true, /*passive=*/false, /*max_ms_per_chan=*/120, channel);
}
bool scanHidden(int i) { return WiFi.SSID(i).isEmpty(); }
bool scanReportsWidth() { return true; }
// From the AP's HT operation element: a secondary channel means it runs 40 MHz (HT40+/-).
void scanWidth(int i, uint8_t& width, int8_t& secondary) {
    auto* r = static_cast<wifi_ap_record_t*>(WiFi.getScanInfoByIndex(i));
    width = 20;
    secondary = 0;
    if (!r) return;
    if (r->second == WIFI_SECOND_CHAN_ABOVE) {
        width = 40;
        secondary = 1;
    } else if (r->second == WIFI_SECOND_CHAN_BELOW) {
        width = 40;
        secondary = -1;
    }
}
bool encIsOpen(uint8_t enc) { return enc == WIFI_AUTH_OPEN; }

// Unlike the ESP8266, the ESP32 scan reports WPA3.
const char* securityName(uint8_t enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN:            return "Open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "Unknown";
    }
}
bool staWrongPassword() { return false; }  // the ESP32 core reports this as a plain connect failure
void startTime(const char* tz, const char* server) { configTzTime(tz, server); }
void mdnsUpdate() {}  // runs in its own task on ESP32

#endif

}  // namespace Platform
