#pragma once
#include <Arduino.h>

// Channel analysis. Everything here is an ESTIMATE from detected APs (count,
// signal, 20 MHz overlap, repeated scans); it is not a measurement of airtime
// or RF utilization and must be labelled that way in every UI/MQTT output.
enum CongestionLevel : uint8_t { LOAD_LOW, LOAD_MEDIUM, LOAD_HIGH, LOAD_VERY_HIGH };

struct ChannelStat {
    uint8_t aps;           // in the latest scan
    uint8_t strong;        // >= -67 dBm
    uint8_t medium;        // -80 .. -68 dBm
    uint8_t weak;          // < -80 dBm
    int8_t avgRssi, maxRssi, minRssi;  // RSSI_NONE when aps == 0
    uint16_t overlapping;  // APs on other channels within +-4 (20 MHz overlap)
    float load;            // estimated load score, averaged over recent scans
    uint8_t level;         // CongestionLevel
};

namespace Stats {

constexpr uint8_t kChannels = 14;

// out[c-1] for channel c. Returns the number of scans the load is averaged over.
uint8_t analyzeChannels(ChannelStat out[kChannels]);

// Lowest estimated load among 1/6/11 (the non-overlapping 20 MHz set).
uint8_t recommendedChannel(const ChannelStat stats[kChannels]);
// Fewest detected APs (latest scan) on channels 1..13; ties -> lower load.
uint8_t leastPopulatedChannel(const ChannelStat stats[kChannels]);

const char* levelName(uint8_t level);        // "LOW", "MEDIUM", "HIGH", "VERY HIGH"
const char* confidenceName(uint8_t scans);   // "LOW", "MEDIUM", "HIGH"

}  // namespace Stats
