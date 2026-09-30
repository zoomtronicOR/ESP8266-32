#include "alerts.h"

#include <LittleFS.h>
#include <time.h>

#include "config.h"
#include "mqtt_manager.h"
#include "platform.h"
#include "storage.h"
#include "system.h"
#include "wifi_manager.h"
#include "wifi_scanner.h"

namespace {

const char* const kTypeNames[ALERT_TYPE_COUNT] = {
    "new_ap",        "open_ap",         "strong_ap",       "channel_density", "wifi_disconnected",
    "mqtt_disconnected", "low_memory",  "watchdog_reset",  "exception_reset", "restart_loop",
};

// ---- ring ----
Alert s_ring[ALERT_RING_SIZE];
uint16_t s_count = 0;
uint16_t s_next = 0;  // write position
uint32_t s_lastId = 0;
uint16_t s_fileRecords = 0;

// ---- boot counters ----
struct __attribute__((packed)) BootFile {
    uint32_t magic;
    BootInfo info;
};
constexpr uint32_t kBootMagic = 0x42543031;  // "BT01"
BootInfo s_boot = {};

// ---- known BSSIDs (bloom filter) ----
constexpr uint32_t kKnownMagic = 0x4b4e4231;  // "KNB1"
uint8_t s_known[KNOWN_BLOOM_BYTES];
bool s_knownDirty = false;
uint8_t s_baselineScans = 0;  // no filter on flash yet: the next scans only learn
uint32_t s_knownSavedMs = 0;

// ---- background state ----
bool s_densityActive[15] = {false};  // index = channel 1..14
bool s_firstScan = true;
uint32_t s_wifiDownSince = 0;
bool s_wifiWasUp = false;
uint32_t s_mqttDownSince = 0;
bool s_mqttEverUp = false;
bool s_mqttAlerted = false;
bool s_lowMemActive = false;
bool s_stableSaved = false;
uint32_t s_lastCheckMs = 0;

void saveBoot() {
    BootFile f = {kBootMagic, s_boot};
    File file = LittleFS.open(BOOT_PATH, "w");
    if (!file) return;
    file.write((const uint8_t*)&f, sizeof(f));
    file.close();
}

void saveKnown() {
    File file = LittleFS.open(KNOWN_PATH, "w");
    if (!file) return;
    file.write((const uint8_t*)&kKnownMagic, sizeof(kKnownMagic));
    file.write(s_known, sizeof(s_known));
    file.close();
    s_knownDirty = false;
    s_knownSavedMs = millis();
}

uint32_t fnv(const uint8_t* b, uint32_t seed) {
    uint32_t h = 2166136261u ^ seed;
    for (int i = 0; i < 6; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// Two bit positions per BSSID
bool knownTest(const uint8_t* bssid, bool set) {
    const uint32_t bits = KNOWN_BLOOM_BYTES * 8;
    uint32_t a = fnv(bssid, 0) % bits, b = fnv(bssid, 0x9e3779b9) % bits;
    bool had = (s_known[a >> 3] & (1 << (a & 7))) && (s_known[b >> 3] & (1 << (b & 7)));
    if (set && !had) {
        s_known[a >> 3] |= 1 << (a & 7);
        s_known[b >> 3] |= 1 << (b & 7);
        s_knownDirty = true;
    }
    return had;
}

void pushRing(const Alert& a) {
    s_ring[s_next] = a;
    s_next = (s_next + 1) % ALERT_RING_SIZE;
    if (s_count < ALERT_RING_SIZE) s_count++;
}

void persist(const Alert& a) {
    // Append; once the file holds 4x the ring, rewrite it with just the ring.
    if (s_fileRecords >= ALERT_RING_SIZE * 4) {
        File f = LittleFS.open(ALERTS_PATH, "w");
        if (!f) return;
        for (uint16_t i = 0; i < s_count; i++) f.write((const uint8_t*)&Alerts::at(i), sizeof(Alert));
        f.close();
        s_fileRecords = s_count;
        return;  // `a` is already the newest ring entry
    }
    File f = LittleFS.open(ALERTS_PATH, "a");
    if (!f) return;
    f.write((const uint8_t*)&a, sizeof(a));
    f.close();
    s_fileRecords++;
}

Alert& raise(AlertType type, AlertSeverity sev, int32_t value = 0) {
    static Alert a;
    memset(&a, 0, sizeof(a));
    a.id = ++s_lastId;
    a.epoch = System::timeSynced() ? (uint32_t)time(nullptr) : 0;
    a.uptime = System::uptimeSeconds();
    a.boot = (uint16_t)s_boot.boots;
    a.type = type;
    a.severity = sev;
    a.value = value;
    return a;
}

void commit(Alert& a) {
    pushRing(a);
    persist(a);
    LOGF("Alert #%lu [%s] %s", (unsigned long)a.id, Alerts::severityName(a.severity),
         Alerts::message(a, true).c_str());
}

void raiseForAp(AlertType type, AlertSeverity sev, const ApRecord& r) {
    Alert& a = raise(type, sev, r.rssi);
    memcpy(a.bssid, r.bssid, 6);
    strlcpy(a.ssid, r.ssid, sizeof(a.ssid));
    a.channel = r.channel;
    a.rssi = r.rssi;
    commit(a);
}

bool densityAlertInRing(uint8_t channel) {
    for (uint16_t i = 0; i < s_count; i++) {
        const Alert& a = Alerts::at(i);
        if (a.type == ALERT_CHANNEL_DENSITY && a.channel == channel) return true;
    }
    return false;
}

void loadAlerts() {
    File f = LittleFS.open(ALERTS_PATH, "r");
    if (!f) return;
    Alert a;
    while (f.read((uint8_t*)&a, sizeof(a)) == sizeof(a)) {
        pushRing(a);
        if (a.id > s_lastId) s_lastId = a.id;
        s_fileRecords++;
    }
    f.close();
}

void bootAlerts() {
    bool watchdog = Platform::resetByWatchdog();
    bool exception = Platform::resetByCrash();
    s_boot.boots++;
    if (watchdog) s_boot.watchdogResets++;
    if (exception) s_boot.exceptionResets++;
    // Power-on, reset button, flashing and our own reboots are normal restarts.
    s_boot.abnormalInARow = (watchdog || exception) ? s_boot.abnormalInARow + 1 : 0;
    saveBoot();

    if (!config.alertSystem) return;
    if (watchdog) commit(raise(ALERT_WATCHDOG, SEV_CRITICAL));
    if (exception) commit(raise(ALERT_EXCEPTION, SEV_CRITICAL, Platform::crashCause()));
    if (s_boot.abnormalInARow >= RESTART_LOOP_COUNT) {
        commit(raise(ALERT_RESTART_LOOP, SEV_CRITICAL, s_boot.abnormalInARow));
    }
}

void checkSystem() {
    uint32_t up = System::uptimeSeconds();
    if (!s_stableSaved && up >= STABLE_UPTIME_S) {
        s_stableSaved = true;
        if (s_boot.abnormalInARow) {
            s_boot.abnormalInARow = 0;
            saveBoot();
        }
    }
    if (!config.alertSystem) return;

    bool wifiUp = WifiManager::staConnected();
    if (!wifiUp && s_wifiWasUp) s_wifiDownSince = up;
    if (wifiUp && !s_wifiWasUp && s_wifiDownSince) {
        uint32_t outage = up - s_wifiDownSince;
        // Raised on reconnect: only then can it reach MQTT/HA, and the duration is known.
        if (outage >= WIFI_OUTAGE_ALERT_S) commit(raise(ALERT_WIFI_DOWN, SEV_WARNING, outage));
        s_wifiDownSince = 0;
    }
    s_wifiWasUp = wifiUp;

    if (Mqtt::connected()) {
        s_mqttEverUp = true;
        s_mqttDownSince = 0;
        s_mqttAlerted = false;
    } else if (Mqtt::enabled() && wifiUp && s_mqttEverUp) {
        if (!s_mqttDownSince) s_mqttDownSince = up;
        if (!s_mqttAlerted && up - s_mqttDownSince >= MQTT_OUTAGE_ALERT_S) {
            s_mqttAlerted = true;
            commit(raise(ALERT_MQTT_DOWN, SEV_WARNING, up - s_mqttDownSince));
        }
    }

    uint32_t heap = ESP.getFreeHeap();
    if (!s_lowMemActive && heap < LOW_MEMORY_BYTES) {
        s_lowMemActive = true;
        commit(raise(ALERT_LOW_MEMORY, SEV_WARNING, heap));
    } else if (s_lowMemActive && heap > LOW_MEMORY_REARM_BYTES) {
        s_lowMemActive = false;
    }
}

}  // namespace

namespace Alerts {

void begin() {
    File f = LittleFS.open(BOOT_PATH, "r");
    if (f) {
        BootFile bf;
        if (f.read((uint8_t*)&bf, sizeof(bf)) == sizeof(bf) && bf.magic == kBootMagic) s_boot = bf.info;
        f.close();
    }

    memset(s_known, 0, sizeof(s_known));
    f = LittleFS.open(KNOWN_PATH, "r");
    uint32_t magic = 0;
    if (f && f.read((uint8_t*)&magic, sizeof(magic)) == sizeof(magic) && magic == kKnownMagic &&
        f.read(s_known, sizeof(s_known)) == sizeof(s_known)) {
        s_baselineScans = 0;
    } else {
        memset(s_known, 0, sizeof(s_known));
        s_baselineScans = KNOWN_BASELINE_SCANS;
    }
    if (f) f.close();

    loadAlerts();
    bootAlerts();
    LOGF("Alerts: %u stored, boot #%lu, %s", s_count, (unsigned long)s_boot.boots,
         s_baselineScans ? "learning known APs" : "known-AP filter loaded");
}

void loop() {
    if (millis() - s_lastCheckMs < 1000) return;
    s_lastCheckMs = millis();
    checkSystem();
    // Never save while the baseline is still learning: after a reboot a partial filter on
    // flash would end the learning phase early and report already-present APs as new.
    if (s_knownDirty && !s_baselineScans &&
        (s_knownSavedMs == 0 || millis() - s_knownSavedMs >= KNOWN_SAVE_INTERVAL_MS))
        saveKnown();
}

void onScan() {
    uint32_t id = Scanner::scanId();
    ApRecord* recs = Scanner::mutableRecords();
    uint8_t newAlerts = 0;
    uint16_t perChannel[15] = {0};

    for (uint16_t i = 0; i < Scanner::count(); i++) {
        ApRecord& r = recs[i];
        if (!Scanner::isPresent(r)) continue;
        if (r.channel >= 1 && r.channel <= 14) perChannel[r.channel]++;
        if (r.firstScanId != id) continue;  // only APs that entered the table in this scan
        if (knownTest(r.bssid, true) || s_baselineScans) continue;

        r.discovered = true;
        if (newAlerts >= MAX_NEW_AP_ALERTS_PER_SCAN) continue;
        newAlerts++;
        if (config.alertNewAp) raiseForAp(ALERT_NEW_AP, SEV_INFO, r);
        if (config.alertOpenAp && Platform::encIsOpen(r.enc)) raiseForAp(ALERT_OPEN_AP, SEV_WARNING, r);
        if (config.alertStrongAp && r.rssi >= config.alertStrongRssi) raiseForAp(ALERT_STRONG_AP, SEV_WARNING, r);
    }
    if (s_baselineScans && --s_baselineScans == 0) {
        saveKnown();
        LOGF("Alerts: baseline of %u known APs saved", Scanner::count());
    }

    // Channel density with hysteresis; after a reboot, do not repeat an alert that is still listed.
    for (uint8_t c = 1; c <= 14; c++) {
        uint16_t n = perChannel[c];
        if (!s_densityActive[c] && n >= config.alertDensityAps) {
            s_densityActive[c] = true;
            if (config.alertDensity && !(s_firstScan && densityAlertInRing(c))) {
                Alert& a = raise(ALERT_CHANNEL_DENSITY, SEV_WARNING, n);
                a.channel = c;
                commit(a);
            }
        } else if (s_densityActive[c] && n + 1 < config.alertDensityAps) {
            s_densityActive[c] = false;
        }
    }
    s_firstScan = false;
}

uint16_t count() { return s_count; }

const Alert& at(uint16_t i) { return s_ring[(s_next + ALERT_RING_SIZE - s_count + i) % ALERT_RING_SIZE]; }

uint32_t lastId() { return s_lastId; }

uint16_t unread() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < s_count; i++) {
        if (at(i).id > s_boot.ackId) n++;
    }
    return n;
}

bool unreadProblem() {
    for (uint16_t i = 0; i < s_count; i++) {
        if (at(i).id > s_boot.ackId && at(i).severity >= SEV_WARNING) return true;
    }
    return false;
}

void ackAll() {
    if (s_boot.ackId == s_lastId) return;
    s_boot.ackId = s_lastId;
    saveBoot();
}

const char* typeName(uint8_t type) { return type < ALERT_TYPE_COUNT ? kTypeNames[type] : "unknown"; }

const char* severityName(uint8_t s) { return s == SEV_CRITICAL ? "critical" : s == SEV_WARNING ? "warning" : "info"; }

String message(const Alert& a, bool withIdentity) {
    String who;
    if (withIdentity) {
        char b[18];
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3],
                 a.bssid[4], a.bssid[5]);
        who = String(' ') + (a.ssid[0] ? String('"') + a.ssid + '"' : String(F("(hidden SSID)"))) + F(" (") + b + ')';
    }
    char buf[96];
    switch (a.type) {
        case ALERT_NEW_AP:
            snprintf(buf, sizeof(buf), "New access point on channel %u, %d dBm:", a.channel, a.rssi);
            return withIdentity ? buf + who : String(buf).substring(0, strlen(buf) - 1);
        case ALERT_OPEN_AP:
            snprintf(buf, sizeof(buf), "Open WiFi network detected on channel %u:", a.channel);
            return withIdentity ? buf + who : String(buf).substring(0, strlen(buf) - 1);
        case ALERT_STRONG_AP:
            snprintf(buf, sizeof(buf), "Very strong nearby AP detected (%d dBm, channel %u):", a.rssi, a.channel);
            return withIdentity ? buf + who : String(buf).substring(0, strlen(buf) - 1);
        case ALERT_CHANNEL_DENSITY:
            snprintf(buf, sizeof(buf), "High AP density on channel %u: %ld access points detected", a.channel,
                     (long)a.value);
            return buf;
        case ALERT_WIFI_DOWN:
            snprintf(buf, sizeof(buf), "WiFi was disconnected for %ld s", (long)a.value);
            return buf;
        case ALERT_MQTT_DOWN:
            snprintf(buf, sizeof(buf), "MQTT broker disconnected for more than %ld s", (long)a.value);
            return buf;
        case ALERT_LOW_MEMORY:
            snprintf(buf, sizeof(buf), "Low memory: %ld bytes free", (long)a.value);
            return buf;
        case ALERT_WATCHDOG:
            return F("Restarted by the watchdog");
        case ALERT_EXCEPTION:
            snprintf(buf, sizeof(buf), "Restarted after a crash (exception %ld)", (long)a.value);
            return buf;
        case ALERT_RESTART_LOOP:
            snprintf(buf, sizeof(buf), "High restart count: %ld abnormal restarts in a row", (long)a.value);
            return buf;
        default:
            return F("Unknown alert");
    }
}

const BootInfo& bootInfo() { return s_boot; }

void flush() {
    if (s_knownDirty && !s_baselineScans) saveKnown();
}

void clear() {
    LittleFS.remove(ALERTS_PATH);
    LittleFS.remove(KNOWN_PATH);
    LittleFS.remove(BOOT_PATH);
    s_count = s_next = 0;
    s_fileRecords = 0;
}

}  // namespace Alerts
