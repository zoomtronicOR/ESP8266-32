#include "wifi_scanner.h"

#include "platform.h"

#include "config.h"
#include "alerts.h"
#include "history.h"
#include "storage.h"
#include "system.h"

namespace {

ApRecord* s_recs = nullptr;
uint16_t s_cap = 0;
uint16_t s_count = 0;

uint32_t s_scanId = 0;
bool s_scanning = false;
bool s_manualPending = false;
uint32_t s_scanStartMs = 0;
uint32_t s_lastScanEndMs = 0;
uint32_t s_lastDurationMs = 0;
uint32_t s_lastScanUptime = 0;

ApRecord* findOrAllocate(const uint8_t* bssid) {
    for (uint16_t i = 0; i < s_count; i++) {
        if (memcmp(s_recs[i].bssid, bssid, 6) == 0) return &s_recs[i];
    }
    ApRecord* slot = nullptr;
    if (s_count < s_cap) {
        slot = &s_recs[s_count++];
    } else {
        // Table full: evict the AP not seen for the longest time, but never one
        // already observed in the scan being processed.
        for (uint16_t i = 0; i < s_count; i++) {
            if (s_recs[i].lastScanId == s_scanId) continue;
            if (!slot || s_recs[i].lastSeen < slot->lastSeen) slot = &s_recs[i];
        }
        if (!slot) return nullptr;
    }
    memset(slot, 0, sizeof(*slot));
    memset(slot->live, (uint8_t)RSSI_NONE, sizeof(slot->live));
    memcpy(slot->bssid, bssid, 6);
    slot->rssiMin = 127;
    slot->rssiMax = -128;
    slot->firstSeen = System::uptimeSeconds();
    slot->firstScanId = s_scanId;
    return slot;
}

void processResults(int n) {
    s_scanId++;
    uint32_t now = System::uptimeSeconds();
    uint16_t dropped = 0;
    for (int i = 0; i < n; i++) {
        ApRecord* r = findOrAllocate(WiFi.BSSID(i));
        if (!r) {
            dropped++;
            continue;
        }
        strlcpy(r->ssid, WiFi.SSID(i).c_str(), sizeof(r->ssid));
        int32_t rssi = WiFi.RSSI(i);
        r->rssi = (int8_t)constrain(rssi, -128, 0);
        if (r->rssi < r->rssiMin) r->rssiMin = r->rssi;
        if (r->rssi > r->rssiMax) r->rssiMax = r->rssi;
        r->channel = (uint8_t)WiFi.channel(i);
        r->enc = (uint8_t)WiFi.encryptionType(i);
        r->hidden = Platform::scanHidden(i);
        if (r->seenCount < UINT16_MAX) {
            r->seenCount++;
            r->rssiSum += r->rssi;
        }
        r->lastSeen = now;
        r->lastScanId = s_scanId;
    }
    // Every tracked AP gets a slot for this scan, RSSI_NONE if it was not seen.
    uint8_t slot = s_scanId % HISTORY_LIVE_POINTS;
    for (uint16_t i = 0; i < s_count; i++) {
        s_recs[i].live[slot] = s_recs[i].lastScanId == s_scanId ? s_recs[i].rssi : RSSI_NONE;
    }
    s_lastDurationMs = millis() - s_scanStartMs;
    s_lastScanUptime = now;
    LOGF("Scan #%lu: %d APs in %lu ms (tracked %u/%u%s)", (unsigned long)s_scanId, n,
         (unsigned long)s_lastDurationMs, s_count, s_cap, dropped ? ", table full" : "");
    History::onScan();
    Alerts::onScan();
}

}  // namespace

namespace Scanner {

void begin(uint16_t cap) {
    s_cap = cap;
    s_recs = new ApRecord[cap];
    s_count = 0;
    LOGF("Scanner: capacity %u APs (%u bytes)", cap, (unsigned)(cap * sizeof(ApRecord)));
}

void loop(bool allowed) {
    if (s_scanning) {
        int8_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (millis() - s_scanStartMs > SCAN_TIMEOUT_MS) {
                LOGF("Scan timed out");
                WiFi.scanDelete();
                s_scanning = false;
                s_lastScanEndMs = millis();
            }
            return;
        }
        if (n >= 0) {
            processResults(n);
        } else {
            LOGF("Scan failed (%d)", n);
        }
        WiFi.scanDelete();
        s_scanning = false;
        s_lastScanEndMs = millis();
        return;
    }

    if (!allowed) return;
    bool due = s_manualPending ||
               (config.autoScan &&
                (s_scanId == 0 || millis() - s_lastScanEndMs >= (uint32_t)config.scanInterval * 1000));
    if (!due) return;

    s_manualPending = false;
    s_scanning = true;
    s_scanStartMs = millis();
    // scanChannel 0 = all channels; 1..13 = only that one (much shorter scan)
    Platform::startScan(config.scanChannel);
}

bool requestScan() {
    if (s_scanning || s_manualPending) return false;
    if (s_scanId > 0 && millis() - s_lastScanEndMs < MANUAL_SCAN_MIN_GAP_MS) return false;
    s_manualPending = true;
    return true;
}

const ApRecord* records() { return s_recs; }
ApRecord* mutableRecords() { return s_recs; }

const ApRecord* find(const uint8_t* bssid) {
    for (uint16_t i = 0; i < s_count; i++) {
        if (memcmp(s_recs[i].bssid, bssid, 6) == 0) return &s_recs[i];
    }
    return nullptr;
}
uint16_t count() { return s_count; }
uint16_t capacity() { return s_cap; }
uint32_t scanId() { return s_scanId; }
bool scanning() { return s_scanning || s_manualPending; }
uint32_t lastScanDurationMs() { return s_lastDurationMs; }
uint32_t lastScanUptime() { return s_lastScanUptime; }

bool isPresent(const ApRecord& r) { return s_scanId > 0 && r.lastScanId == s_scanId; }

bool isNew(const ApRecord& r) {
    // "New" = never seen before by this device (persisted across reboots), for a while.
    return r.discovered && System::uptimeSeconds() - r.firstSeen < NEW_AP_WINDOW_S;
}

ScanSummary summary() {
    ScanSummary s = {};
    s.strongest = -128;
    uint16_t channelMask = 0;
    for (uint16_t i = 0; i < s_count; i++) {
        const ApRecord& r = s_recs[i];
        if (!isPresent(r)) continue;
        s.present++;
        if (Platform::encIsOpen(r.enc)) s.open++;
        if (r.hidden) s.hidden++;
        if (r.rssi >= STRONG_RSSI_DBM) s.strong++;
        if (isNew(r)) s.isNew++;
        if (r.rssi > s.strongest) s.strongest = r.rssi;
        if (r.channel >= 1 && r.channel <= 14) channelMask |= (1 << r.channel);
    }
    s.channelsUsed = __builtin_popcount(channelMask);
    return s;
}

}  // namespace Scanner
