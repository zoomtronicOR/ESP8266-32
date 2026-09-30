#pragma once
#include <Arduino.h>

#include <functional>

// Device-level snapshot. In the RAM ring `t` is uptime seconds; in flash files it
// is the UTC epoch of the bucket start. Averages over the scans in the bucket.
struct __attribute__((packed)) HistorySummary {
    uint32_t t;
    uint8_t aps;
    uint8_t open;
    uint8_t hidden;
    int8_t strongest;  // RSSI_NONE when no AP was seen
    int8_t avgRssi;    // RSSI_NONE when no AP was seen
    uint8_t scans;
    uint8_t ch[14];    // APs per channel 1..14
};
static_assert(sizeof(HistorySummary) == 24, "on-flash record layout");

// History engine:
//  - RAM ring of the last HISTORY_LIVE_POINTS scans (works without NTP)
//  - LittleFS: one summary record + one per-AP RSSI frame per HISTORY_BUCKET_S,
//    in per-day files /hist/s<day>.bin and /hist/a<day>.bin (day = epoch / 86400),
//    pruned by the history_hours setting and by free space.
namespace History {

void begin();
void loop();
void onScan();  // called by the scanner after each completed scan

// RAM ring: valid for scan ids (scanId - liveCount() + 1) .. scanId
uint16_t liveCount();
const HistorySummary& liveAt(uint32_t scanId);

bool persistent();  // flash history active (time is synced)

// Flash readers, oldest first; records with t >= fromEpoch. They yield() while reading.
void forEachSummary(uint32_t fromEpoch, const std::function<void(const HistorySummary&)>& cb);
void forEachApPoint(const uint8_t* bssid, uint32_t fromEpoch,
                    const std::function<void(uint32_t t, int8_t rssi, uint8_t channel)>& cb);

void prune();
void clear();  // factory reset

}  // namespace History
