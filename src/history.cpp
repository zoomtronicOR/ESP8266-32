#include "history.h"

#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <time.h>

#include <vector>

#include "config.h"
#include "storage.h"
#include "system.h"
#include "wifi_scanner.h"

namespace {

struct __attribute__((packed)) FrameHeader {
    uint32_t t;  // bucket start, UTC epoch
    uint8_t n;   // number of FrameEntry that follow
    uint8_t reserved;
};

struct __attribute__((packed)) FrameEntry {
    uint8_t bssid[6];
    int8_t rssi;  // average over the bucket
    uint8_t channel;
};

HistorySummary s_live[HISTORY_LIVE_POINTS];

// Accumulators for the flash bucket in progress
uint32_t s_bucket = 0;  // epoch / HISTORY_BUCKET_S, 0 = none open
uint16_t s_bScans = 0;
uint32_t s_bAps = 0, s_bOpen = 0, s_bHidden = 0;
int32_t s_bStrongest = 0, s_bAvg = 0;
uint16_t s_bRssiScans = 0;  // scans that saw at least one AP
uint16_t s_bCh[14];

uint32_t s_lastCheckMs = 0;


String dayPath(char kind, uint32_t day) {
    char p[24];
    snprintf(p, sizeof(p), HISTORY_DIR "/%c%05lu.bin", kind, (unsigned long)day);
    return String(p);
}

uint32_t nowEpoch() { return (uint32_t)time(nullptr); }

HistorySummary snapshot() {
    HistorySummary h = {};
    h.strongest = RSSI_NONE;
    int32_t sum = 0;
    const ApRecord* recs = Scanner::records();
    for (uint16_t i = 0; i < Scanner::count(); i++) {
        const ApRecord& r = recs[i];
        if (!Scanner::isPresent(r)) continue;
        if (h.aps < 255) h.aps++;
        if (r.enc == ENC_TYPE_NONE && h.open < 255) h.open++;
        if (r.hidden && h.hidden < 255) h.hidden++;
        if (r.rssi > h.strongest) h.strongest = r.rssi;
        sum += r.rssi;
        if (r.channel >= 1 && r.channel <= 14 && h.ch[r.channel - 1] < 255) h.ch[r.channel - 1]++;
    }
    h.avgRssi = h.aps ? (int8_t)lroundf((float)sum / h.aps) : RSSI_NONE;
    h.scans = 1;
    return h;
}

void resetBucket() {
    s_bScans = 0;
    s_bAps = s_bOpen = s_bHidden = 0;
    s_bStrongest = s_bAvg = 0;
    s_bRssiScans = 0;
    memset(s_bCh, 0, sizeof(s_bCh));
    ApRecord* recs = Scanner::mutableRecords();
    for (uint16_t i = 0; i < Scanner::count(); i++) {
        recs[i].bucketSum = 0;
        recs[i].bucketCount = 0;
    }
}

void flushBucket() {
    if (s_bScans == 0) {
        resetBucket();
        return;
    }
    auto avg = [](uint32_t sum, uint16_t n) { return (uint8_t)min<uint32_t>(255, (sum + n / 2) / n); };

    HistorySummary h = {};
    h.t = s_bucket * HISTORY_BUCKET_S;
    h.scans = (uint8_t)min<uint16_t>(255, s_bScans);
    h.aps = avg(s_bAps, s_bScans);
    h.open = avg(s_bOpen, s_bScans);
    h.hidden = avg(s_bHidden, s_bScans);
    h.strongest = s_bRssiScans ? (int8_t)lroundf((float)s_bStrongest / s_bRssiScans) : RSSI_NONE;
    h.avgRssi = s_bRssiScans ? (int8_t)lroundf((float)s_bAvg / s_bRssiScans) : RSSI_NONE;
    for (int c = 0; c < 14; c++) h.ch[c] = avg(s_bCh[c], s_bScans);

    uint32_t day = h.t / 86400;
    File f = LittleFS.open(dayPath('s', day), "a");
    if (f) {
        f.write((const uint8_t*)&h, sizeof(h));
        f.close();
    }

    // Per-AP frame: every AP seen at least once in this bucket
    // Entries are written one by one (LittleFS buffers them), so no frame-sized RAM buffer.
    FrameHeader hdr = {h.t, 0, 0};
    const ApRecord* recs = Scanner::records();
    for (uint16_t i = 0; i < Scanner::count() && hdr.n < 255; i++) {
        if (recs[i].bucketCount) hdr.n++;
    }
    f = LittleFS.open(dayPath('a', day), "a");
    if (f) {
        f.write((const uint8_t*)&hdr, sizeof(hdr));
        uint8_t written = 0;
        for (uint16_t i = 0; i < Scanner::count() && written < hdr.n; i++) {
            const ApRecord& r = recs[i];
            if (!r.bucketCount) continue;
            FrameEntry e;
            memcpy(e.bssid, r.bssid, 6);
            e.rssi = (int8_t)lroundf((float)r.bucketSum / r.bucketCount);
            e.channel = r.channel;
            f.write((const uint8_t*)&e, sizeof(e));
            written++;
        }
        f.close();
    }
    LOGF("History: bucket saved (%u scans, %u APs)", s_bScans, hdr.n);
    resetBucket();
    History::prune();
}

// Day number parsed from "s12345.bin" / "a12345.bin", 0 if not a history file.
uint32_t fileDay(const String& name) {
    if (name.length() < 6 || (name[0] != 's' && name[0] != 'a')) return 0;
    return strtoul(name.c_str() + 1, nullptr, 10);
}

size_t fsFree() {
    FSInfo info;
    LittleFS.info(info);
    return info.totalBytes - info.usedBytes;
}

}  // namespace

namespace History {

void begin() {
    LittleFS.mkdir(HISTORY_DIR);
    memset(s_live, 0, sizeof(s_live));
    memset(s_bCh, 0, sizeof(s_bCh));
}

void loop() {
    // A bucket may also end without a scan (long scan intervals, scanning paused).
    if (millis() - s_lastCheckMs < 5000) return;
    s_lastCheckMs = millis();
    if (s_bucket && persistent() && nowEpoch() / HISTORY_BUCKET_S != s_bucket) {
        flushBucket();
        s_bucket = 0;
    }
}

void onScan() {
    HistorySummary h = snapshot();
    h.t = System::uptimeSeconds();
    s_live[Scanner::scanId() % HISTORY_LIVE_POINTS] = h;

    if (!persistent()) return;
    uint32_t bucket = nowEpoch() / HISTORY_BUCKET_S;
    if (s_bucket && bucket != s_bucket) flushBucket();
    if (bucket != s_bucket) {
        if (!s_bucket) resetBucket();  // drop per-AP sums gathered before the first bucket
        s_bucket = bucket;
    }

    s_bScans++;
    s_bAps += h.aps;
    s_bOpen += h.open;
    s_bHidden += h.hidden;
    for (int c = 0; c < 14; c++) s_bCh[c] += h.ch[c];
    if (h.aps) {
        s_bStrongest += h.strongest;
        s_bAvg += h.avgRssi;
        s_bRssiScans++;
    }
    ApRecord* recs = Scanner::mutableRecords();
    for (uint16_t i = 0; i < Scanner::count(); i++) {
        ApRecord& r = recs[i];
        if (!Scanner::isPresent(r) || r.bucketCount == 255) continue;
        r.bucketSum += r.rssi;
        r.bucketCount++;
    }
}

uint16_t liveCount() { return (uint16_t)min<uint32_t>(Scanner::scanId(), HISTORY_LIVE_POINTS); }

const HistorySummary& liveAt(uint32_t scanId) { return s_live[scanId % HISTORY_LIVE_POINTS]; }

bool persistent() { return System::timeSynced(); }

void forEachSummary(uint32_t fromEpoch, const std::function<void(const HistorySummary&)>& cb) {
    if (!persistent()) return;
    uint32_t today = nowEpoch() / 86400;
    uint16_t n = 0;
    for (uint32_t day = fromEpoch / 86400; day <= today; day++) {
        File f = LittleFS.open(dayPath('s', day), "r");
        if (!f) continue;
        HistorySummary h;
        while (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)) {
            if (h.t >= fromEpoch) cb(h);
            if (++n % 64 == 0) yield();
        }
        f.close();
    }
}

void forEachApPoint(const uint8_t* bssid, uint32_t fromEpoch,
                    const std::function<void(uint32_t, int8_t, uint8_t)>& cb) {
    if (!persistent()) return;
    uint32_t today = nowEpoch() / 86400;
    for (uint32_t day = fromEpoch / 86400; day <= today; day++) {
        File f = LittleFS.open(dayPath('a', day), "r");
        if (!f) continue;
        FrameHeader hdr;
        FrameEntry buf[16];  // frame read in small chunks
        while (f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr)) {
            if (hdr.t < fromEpoch) {
                if (!f.seek(hdr.n * sizeof(FrameEntry), SeekCur)) break;
                continue;
            }
            bool found = false;
            for (uint8_t left = hdr.n; left > 0;) {
                uint8_t k = min<uint8_t>(left, 16);
                if (f.read((uint8_t*)buf, k * sizeof(FrameEntry)) != (int)(k * sizeof(FrameEntry))) return f.close();
                left -= k;
                for (uint8_t i = 0; i < k && !found; i++) {
                    if (memcmp(buf[i].bssid, bssid, 6) == 0) {
                        cb(hdr.t, buf[i].rssi, buf[i].channel);
                        found = true;
                    }
                }
            }
            yield();
        }
        f.close();
    }
}

// Collects history file names first: removing entries while a Dir is being
// iterated is not safe on LittleFS.
std::vector<String> listFiles() {
    std::vector<String> names;
    Dir dir = LittleFS.openDir(HISTORY_DIR);
    while (dir.next()) names.push_back(dir.fileName());
    return names;
}

void prune() {
    if (!persistent()) return;
    uint32_t today = nowEpoch() / 86400;
    uint32_t oldestKept = (nowEpoch() - (uint32_t)config.historyHours * 3600) / 86400;
    uint32_t oldestDay = today;
    for (const String& name : listFiles()) {
        uint32_t day = fileDay(name);
        if (!day) continue;
        if (day < oldestKept) {
            LittleFS.remove(String(HISTORY_DIR "/") + name);
            LOGF("History: removed %s (retention)", name.c_str());
        } else if (day < oldestDay) {
            oldestDay = day;
        }
    }
    // Space guard: drop whole oldest days, but never today's files.
    while (fsFree() < HISTORY_MIN_FREE_BYTES && oldestDay < today) {
        LittleFS.remove(dayPath('s', oldestDay));
        LittleFS.remove(dayPath('a', oldestDay));
        LOGF("History: removed day %lu (low space)", (unsigned long)oldestDay);
        oldestDay++;
    }
}

void clear() {
    for (const String& name : listFiles()) LittleFS.remove(String(HISTORY_DIR "/") + name);
    s_bucket = 0;
}

}  // namespace History
