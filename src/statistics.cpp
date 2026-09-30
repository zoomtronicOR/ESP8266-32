#include "statistics.h"

#include "config.h"
#include "history.h"
#include "wifi_scanner.h"

namespace {

// Channels an AP's signal is centred on: its primary channel, extended by 4 channels
// (20 MHz) towards the secondary for 40 MHz. Unknown width counts as 20 MHz.
void apBlock(const ApRecord& r, int& lo, int& hi) {
    lo = hi = r.channel;
    if (r.width == 40) {
        if (r.secondary > 0) hi += 4;
        if (r.secondary < 0) lo -= 4;
    }
}

// Share of channel c that the AP's spectrum covers. Each 20 MHz block is modelled as
// +-2.5 channels (~22 MHz), so for 20 MHz this is 1 - 0.2*d (d = channels apart).
float overlapWeight(int c, int lo, int hi) {
    float a = max(c - 2.5f, lo - 2.5f), b = min(c + 2.5f, hi + 2.5f);
    return b > a ? (b - a) / 5.0f : 0.0f;
}

// Stronger neighbours cost more airtime/contention than barely audible ones.
float signalWeight(int8_t rssi) {
    if (rssi >= STRONG_RSSI_DBM) return 1.0f;
    if (rssi >= -80) return 0.5f;
    return 0.2f;
}

uint8_t levelFor(float load) {
    if (load < 1.0f) return LOAD_LOW;
    if (load < 3.0f) return LOAD_MEDIUM;
    if (load < 6.0f) return LOAD_HIGH;
    return LOAD_VERY_HIGH;
}

}  // namespace

namespace Stats {

uint8_t analyzeChannels(ChannelStat out[kChannels]) {
    memset(out, 0, sizeof(ChannelStat) * kChannels);
    int32_t sum[kChannels] = {0};
    for (uint8_t c = 0; c < kChannels; c++) out[c].avgRssi = out[c].maxRssi = out[c].minRssi = RSSI_NONE;

    const ApRecord* recs = Scanner::records();
    uint16_t n = Scanner::count();

    // Latest scan: counts and RSSI spread
    for (uint16_t i = 0; i < n; i++) {
        const ApRecord& r = recs[i];
        if (!Scanner::isPresent(r) || r.channel < 1 || r.channel > kChannels) continue;
        ChannelStat& s = out[r.channel - 1];
        s.aps++;
        if (r.rssi >= STRONG_RSSI_DBM) s.strong++;
        else if (r.rssi >= -80) s.medium++;
        else s.weak++;
        sum[r.channel - 1] += r.rssi;
        if (s.maxRssi == RSSI_NONE || r.rssi > s.maxRssi) s.maxRssi = r.rssi;
        if (s.minRssi == RSSI_NONE || r.rssi < s.minRssi) s.minRssi = r.rssi;
    }
    for (uint8_t c = 0; c < kChannels; c++) {
        if (out[c].aps) out[c].avgRssi = (int8_t)lroundf((float)sum[c] / out[c].aps);
    }
    // Overlapping: APs on other primary channels whose spectrum reaches this channel
    for (uint16_t i = 0; i < n; i++) {
        const ApRecord& r = recs[i];
        if (!Scanner::isPresent(r) || r.channel < 1 || r.channel > kChannels) continue;
        int lo, hi;
        apBlock(r, lo, hi);
        for (int c = max(1, lo - 4); c <= min((int)kChannels, hi + 4); c++) {
            if (c != r.channel && overlapWeight(c, lo, hi) > 0) out[c - 1].overlapping++;
        }
    }

    // Load: averaged over the last scans kept in each AP's live RSSI ring
    uint32_t id = Scanner::scanId();
    uint8_t scans = (uint8_t)min<uint32_t>(History::liveCount(), CONGESTION_SCANS);
    for (uint8_t k = 0; k < scans; k++) {
        uint8_t slot = (id - k) % HISTORY_LIVE_POINTS;
        for (uint16_t i = 0; i < n; i++) {
            const ApRecord& r = recs[i];
            int8_t v = r.live[slot];
            if (v == RSSI_NONE || r.channel < 1 || r.channel > 13) continue;
            float sw = signalWeight(v);
            int lo, hi;
            apBlock(r, lo, hi);
            for (int c = max(1, lo - 4); c <= min(13, hi + 4); c++) {
                out[c - 1].load += overlapWeight(c, lo, hi) * sw;
            }
        }
    }
    for (uint8_t c = 0; c < kChannels; c++) {
        if (scans) out[c].load /= scans;
        out[c].level = levelFor(out[c].load);
    }
    return scans;
}

uint8_t recommendedChannel(const ChannelStat s[kChannels]) {
    uint8_t best = 1;
    for (uint8_t c : {6, 11}) {
        if (s[c - 1].load < s[best - 1].load - 0.05f) best = c;
    }
    return best;
}

uint8_t leastPopulatedChannel(const ChannelStat s[kChannels]) {
    uint8_t best = 1;
    for (uint8_t c = 2; c <= 13; c++) {
        const ChannelStat &a = s[c - 1], &b = s[best - 1];
        if (a.aps < b.aps || (a.aps == b.aps && a.load < b.load)) best = c;
    }
    return best;
}

const char* levelName(uint8_t level) {
    switch (level) {
        case LOAD_LOW:    return "LOW";
        case LOAD_MEDIUM: return "MEDIUM";
        case LOAD_HIGH:   return "HIGH";
        default:          return "VERY HIGH";
    }
}

const char* confidenceName(uint8_t scans) { return scans >= 10 ? "HIGH" : scans >= 4 ? "MEDIUM" : "LOW"; }

}  // namespace Stats
