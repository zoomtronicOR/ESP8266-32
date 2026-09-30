#pragma once
#include <Arduino.h>

enum AlertType : uint8_t {
    ALERT_NEW_AP,
    ALERT_OPEN_AP,
    ALERT_STRONG_AP,
    ALERT_CHANNEL_DENSITY,
    ALERT_WIFI_DOWN,
    ALERT_MQTT_DOWN,
    ALERT_LOW_MEMORY,
    ALERT_WATCHDOG,
    ALERT_EXCEPTION,
    ALERT_RESTART_LOOP,
    ALERT_TYPE_COUNT
};

enum AlertSeverity : uint8_t { SEV_INFO, SEV_WARNING, SEV_CRITICAL };

// Fixed-size record: kept in a RAM ring and appended to ALERTS_PATH as is.
struct __attribute__((packed)) Alert {
    uint32_t id;       // increasing, survives reboots
    uint32_t epoch;    // UTC time, 0 when NTP was not synced yet
    uint32_t uptime;   // System::uptimeSeconds() at creation
    uint16_t boot;     // boot number, to tell whether `uptime` is from this boot
    uint8_t type;      // AlertType
    uint8_t severity;  // AlertSeverity
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    int32_t value;     // type specific: APs on channel, seconds offline, free bytes, ...
};

struct BootInfo {
    uint32_t boots;
    uint32_t watchdogResets;
    uint32_t exceptionResets;
    uint8_t abnormalInARow;  // watchdog/exception restarts without a stable run in between
    uint32_t ackId;          // alerts with id > ackId are unread
};

// Alert engine: detects events after each scan and in the background, keeps the
// latest ALERT_RING_SIZE alerts (persisted), and remembers every BSSID ever seen
// (bloom filter in flash) so "new access point" survives reboots.
namespace Alerts {

void begin();  // after Storage and before the scanner starts
void loop();
void onScan();  // called by the scanner after History::onScan()

uint16_t count();
const Alert& at(uint16_t i);  // 0 = oldest
uint32_t lastId();
uint16_t unread();
bool unreadProblem();  // unread warning/critical -> HA "Alert" binary sensor
void ackAll();

const char* typeName(uint8_t type);
const char* severityName(uint8_t severity);
// Human-readable text; withIdentity=false leaves out SSID/BSSID (MQTT privacy).
String message(const Alert& a, bool withIdentity);

const BootInfo& bootInfo();
void flush();  // write pending known-BSSID changes (before a reboot)
void clear();  // factory reset

}  // namespace Alerts
