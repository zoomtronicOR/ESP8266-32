#pragma once
#include <Arduino.h>

#include "config.h"

#define RSSI_NONE INT8_MIN  // "not seen" marker in RSSI history slots

// One tracked access point, keyed by BSSID. Times are System::uptimeSeconds().
struct ApRecord {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;       // last observed
    int8_t rssiMin;
    int8_t rssiMax;
    uint8_t channel;
    uint8_t enc;       // core encryption type, see Platform::securityName
    bool hidden;
    bool discovered;   // never seen before (not in the persisted known-BSSID filter)
    uint16_t seenCount;
    int32_t rssiSum;   // for the average: rssiSum / seenCount
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t firstScanId;
    uint32_t lastScanId;
    // RSSI per scan, slot = scanId % HISTORY_LIVE_POINTS, RSSI_NONE when not seen
    int8_t live[HISTORY_LIVE_POINTS];
    // accumulator for the current flash history bucket (see history.cpp)
    int16_t bucketSum;
    uint8_t bucketCount;
};

struct ScanSummary {
    uint16_t present;       // APs seen in the latest scan
    uint16_t open;
    uint16_t hidden;
    uint16_t strong;
    uint16_t isNew;
    uint8_t channelsUsed;
    int8_t strongest;       // valid only when present > 0
};

namespace Scanner {

void begin(uint16_t capacity);
// Drives the async scan state machine; `allowed` is false while the STA link
// is being (re)established, since a scan would disrupt it.
void loop(bool allowed);

// Queues a manual scan. False if one is running or the last ended too recently.
bool requestScan();

const ApRecord* records();
ApRecord* mutableRecords();  // for History's bucket accumulators only
uint16_t count();
uint16_t capacity();
const ApRecord* find(const uint8_t* bssid);

uint32_t scanId();            // number of completed scans; 0 = none yet
bool scanning();
uint32_t lastScanDurationMs();
uint32_t lastScanUptime();    // uptime seconds of the last completed scan

bool isPresent(const ApRecord& r);
bool isNew(const ApRecord& r);
ScanSummary summary();

}  // namespace Scanner
