#include "web_server.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#if defined(ESP8266)
#include <Updater.h>
#else
#include <Update.h>
#endif

#include "alerts.h"
#include "ble_manager.h"
#include "build_info.h"
#include "config.h"
#include "history.h"
#include "platform.h"
#include "statistics.h"
#include "mqtt_manager.h"
#include "storage.h"
#include "system.h"
#include "wifi_manager.h"
#include "wifi_scanner.h"
#include "wifi_utils.h"

namespace {

WebServerT server(HTTP_PORT);

#ifndef vsnprintf_P
#define vsnprintf_P vsnprintf
#endif

String updateError() {
#if defined(ESP8266)
    return Update.getErrorString();
#else
    return Update.errorString();
#endif
}

// SPA routes: all serve index.html, the JS picks the view from the path.
const char* const kPages[] = {"/", "/networks", "/channels", "/history", "/alerts", "/wifi", "/ble", "/mqtt", "/update", "/settings", "/system"};

// URLs phones/PCs probe to detect a captive portal; answering with a redirect
// makes them pop up the WiFi setup page automatically.
const char* const kCaptiveProbes[] = {"/generate_204",   "/gen_204",       "/hotspot-detect.html",
                                      "/library/test/success.html",       "/ncsi.txt",
                                      "/connecttest.txt", "/redirect",     "/fwlink",
                                      "/canonical.html", "/success.txt"};

// Streams a response in ~1 KB chunks so large JSON never sits in RAM whole.
class ChunkWriter {
public:
    explicit ChunkWriter(const __FlashStringHelper* type) {
        server.sendHeader(F("Cache-Control"), F("no-store"));
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, type, emptyString);
        buf_.reserve(1200);
    }
    String& buf() { return buf_; }
    void add(const char* s) { buf_ += s; maybeFlush(); }
    void addf(const char* fmtP, ...) {
        char tmp[256];
        va_list ap;
        va_start(ap, fmtP);
        vsnprintf_P(tmp, sizeof(tmp), fmtP, ap);
        va_end(ap);
        add(tmp);
    }
    void maybeFlush() {
        if (buf_.length() > 1000) {
            server.sendContent(buf_);
            buf_ = "";
        }
    }
    void end() {
        server.sendContent(buf_);
        server.sendContent(emptyString);  // terminates the chunked response
    }

private:
    String buf_;
};

void sendJson(int code, const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.send(code, F("application/json"), out);
}

void sendError(int code, const __FlashStringHelper* msg) {
    JsonDocument doc;
    doc["error"] = msg;
    sendJson(code, doc);
}

void sendErrorText(int code, const String& msg) {
    JsonDocument doc;
    doc["error"] = msg;
    sendJson(code, doc);
}

void sendOk() {
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(200, doc);
}

bool checkCsrf() {
    if (server.header(CSRF_HEADER) == CSRF_HEADER_VALUE) return true;
    sendError(403, F("Missing " CSRF_HEADER " header"));
    return false;
}

void setIsoOrNull(JsonVariant v, uint32_t atUptime) {
    char iso[24];
    if (System::formatIsoTime(atUptime, iso, sizeof(iso))) {
        v.set(iso);
    } else {
        v.set(nullptr);
    }
}

bool parseBssid(const String& s, uint8_t* out) {
    unsigned int b[6];
    if (s.length() != 17 ||
        sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

// Writes an RSSI as a JSON number, or null for RSSI_NONE.
const char* rssiJson(int8_t v, char* tmp) {
    if (v == RSSI_NONE) return "null";
    itoa(v, tmp, 10);
    return tmp;
}

// ---- Pages / captive portal -------------------------------------------------

void streamPage(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(500, F("text/plain"), F("Web UI files missing. Run: python tools/update_web.py"));
        return;
    }
    server.streamFile(f, F("text/html"));
    f.close();
}

void handleIndex() { streamPage("/index.html"); }

// Chart-only page for iframes, e.g. a Home Assistant Webpage card
void handleEmbed() { streamPage("/embed.html"); }

void redirectToSetup() {
    server.sendHeader(F("Location"), String(F("http://")) + WiFi.softAPIP().toString() + F("/wifi"));
    server.send(302, F("text/plain"), emptyString);
}

// While the hotspot is up, a request for any foreign host name came through our
// wildcard DNS: send it to the setup page.
bool captiveRedirectNeeded() {
    if (!WifiManager::apActive()) return false;
    String host = server.hostHeader();
    return host != WiFi.softAPIP().toString() && host != WiFi.localIP().toString() &&
           host != config.hostname + F(".local") && host != config.hostname;
}

// ---- API: read -----------------------------------------------------------------

void handleStatus() {
    JsonDocument d;
    d["device_name"] = config.deviceName;
    d["product"] = FW_NAME;
    d["firmware"] = FW_VERSION;
    d["build_time"] = BUILD_TIME;
    d["uptime"] = System::uptimeSeconds();
    setIsoOrNull(d["time"].to<JsonVariant>(), System::uptimeSeconds());
    if (System::timeSynced()) {
        d["epoch"] = (uint32_t)time(nullptr);
    } else {
        d["epoch"] = nullptr;
    }

    JsonObject sys = d["system"].to<JsonObject>();
    sys["board"] = BOARD_NAME;
    sys["board_id"] = BOARD_ID;
    sys["chip"] = CHIP_NAME;
    sys["chip_id"] = String(Platform::chipId(), HEX);
    sys["cpu_mhz"] = ESP.getCpuFreqMHz();
    sys["flash_size"] = Platform::flashSize();
    sys["sketch_size"] = ESP.getSketchSize();
    sys["free_sketch"] = ESP.getFreeSketchSpace();
    sys["sdk"] = ESP.getSdkVersion();
    sys["core"] = Platform::coreVersion();
    sys["ram_total"] = Platform::heapTotal();
    sys["free_heap"] = ESP.getFreeHeap();
    sys["heap_fragmentation"] = Platform::heapFragmentation();
    sys["max_free_block"] = Platform::maxFreeBlock();
    sys["reset_reason"] = Platform::resetReason();
    sys["fs_total"] = Platform::fsTotal();
    sys["fs_used"] = Platform::fsUsed();

    JsonObject wifi = d["wifi"].to<JsonObject>();
    bool connected = WifiManager::staConnected();
    wifi["mode"] = WifiManager::modeName();
    wifi["connected"] = connected;
    wifi["ssid"] = connected ? WiFi.SSID() : config.wifiSsid;
    wifi["hostname"] = config.hostname;
    wifi["mac"] = WiFi.macAddress();
    if (connected) {
        wifi["ip"] = WiFi.localIP().toString();
        wifi["rssi"] = WiFi.RSSI();
        wifi["channel"] = WiFi.channel();
    }
    wifi["ap_active"] = WifiManager::apActive();
    if (WifiManager::apActive()) {
        wifi["ap_ssid"] = WifiManager::apSsid();
        wifi["ap_ip"] = WiFi.softAPIP().toString();
        wifi["ap_clients"] = WiFi.softAPgetStationNum();
    }
    wifi["attempt_state"] = WifiManager::attemptState();
    wifi["attempt_ssid"] = WifiManager::attemptSsid();

    JsonObject scan = d["scan"].to<JsonObject>();
    scan["id"] = Scanner::scanId();
    scan["scanning"] = Scanner::scanning();
    scan["allowed"] = WifiManager::scanAllowed();
    scan["auto"] = config.autoScan;
    scan["interval"] = config.scanInterval;
    scan["last_duration_ms"] = Scanner::lastScanDurationMs();
    if (Scanner::scanId() > 0) {
        scan["last_scan_ago"] = System::uptimeSeconds() - Scanner::lastScanUptime();
    } else {
        scan["last_scan_ago"] = nullptr;
    }
    scan["tracked"] = Scanner::count();
    scan["max_aps"] = Scanner::capacity();
    scan["strong_rssi"] = STRONG_RSSI_DBM;

    JsonObject ui = d["ui"].to<JsonObject>();
    ui["theme"] = config.uiTheme;
    ui["accent"] = config.uiAccent;
    d["busy"] = System::busy();

    JsonObject ble = d["ble"].to<JsonObject>();
    ble["available"] = Ble::available();
    if (Ble::available()) {
        ble["enabled"] = Ble::enabled();
        ble["advertising"] = Ble::advertising();
        ble["address"] = Ble::address();
        ble["name"] = config.bleName;
        ble["bthome"] = config.bleBthome;
        ble["scan"] = config.bleScan;
        ble["scan_id"] = Ble::scanId();
        ble["devices"] = Ble::lastScanDevices();
        if (Ble::scanId()) {
            ble["last_scan_ago"] = System::uptimeSeconds() - Ble::lastScanUptime();
        } else {
            ble["last_scan_ago"] = nullptr;
        }
    }

    JsonObject al = d["alerts"].to<JsonObject>();
    al["unread"] = Alerts::unread();
    al["last_id"] = Alerts::lastId();
    al["problem"] = Alerts::unreadProblem();

    const BootInfo& bi = Alerts::bootInfo();
    sys["boots"] = bi.boots;
    sys["watchdog_resets"] = bi.watchdogResets;
    sys["exception_resets"] = bi.exceptionResets;

    JsonObject mq = d["mqtt"].to<JsonObject>();
    mq["enabled"] = config.mqttEnabled;
    mq["connected"] = Mqtt::connected();
    mq["state"] = Mqtt::stateText();
    mq["broker"] = config.mqttHost.length() ? config.mqttHost + ':' + config.mqttPort : String();
    mq["base_topic"] = Mqtt::baseTopic();
    mq["published"] = Mqtt::publishedCount();
    mq["discovery"] = config.mqttDiscovery;

    JsonObject hist = d["history"].to<JsonObject>();
    hist["persistent"] = History::persistent();
    hist["hours"] = config.historyHours;
    hist["bucket_s"] = HISTORY_BUCKET_S;
    hist["live_points"] = HISTORY_LIVE_POINTS;

    ScanSummary s = Scanner::summary();
    JsonObject sum = d["summary"].to<JsonObject>();
    sum["detected"] = s.present;
    sum["open"] = s.open;
    sum["hidden"] = s.hidden;
    sum["strong"] = s.strong;
    sum["new"] = s.isNew;
    sum["channels_used"] = s.channelsUsed;
    if (s.present) {
        sum["strongest_rssi"] = s.strongest;
    } else {
        sum["strongest_rssi"] = nullptr;
    }

    sendJson(200, d);
}

void handleNetworks() {
    bool presentOnly = server.arg(F("present")) == "1";
    long offset = server.hasArg(F("offset")) ? server.arg(F("offset")).toInt() : 0;
    long limit = server.hasArg(F("limit")) ? server.arg(F("limit")).toInt() : Scanner::capacity();
    if (offset < 0) offset = 0;
    if (limit < 1) limit = 1;

    const ApRecord* recs = Scanner::records();
    uint16_t n = Scanner::count();
    uint16_t total = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (!presentOnly || Scanner::isPresent(recs[i])) total++;
    }

    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"scan_id\":%lu,\"total\":%u,\"offset\":%ld,\"networks\":["),
           (unsigned long)Scanner::scanId(), total, offset);

    uint32_t now = System::uptimeSeconds();
    long index = -1;
    long emitted = 0;
    for (uint16_t i = 0; i < n && emitted < limit; i++) {
        const ApRecord& r = recs[i];
        if (presentOnly && !Scanner::isPresent(r)) continue;
        if (++index < offset) continue;

        char bssid[18], first[24], last[24];
        formatBssid(r.bssid, bssid);
        bool haveTime = System::formatIsoTime(r.firstSeen, first, sizeof(first));
        System::formatIsoTime(r.lastSeen, last, sizeof(last));
        int avg = r.seenCount ? (int)lroundf((float)r.rssiSum / r.seenCount) : r.rssi;

        if (emitted++) w.buf() += ',';
        w.buf() += F("{\"ssid\":");
        appendJsonString(w.buf(), r.ssid);
        w.addf(PSTR(",\"bssid\":\"%s\",\"hidden\":%s,\"channel\":%u,\"frequency\":%u,"
                    "\"rssi\":%d,\"rssi_min\":%d,\"rssi_max\":%d,\"rssi_avg\":%d,"
                    "\"security\":\"%s\",\"seen\":%u,\"present\":%s,\"new\":%s,"),
               bssid, r.hidden ? "true" : "false", r.channel, channelToFrequency(r.channel), r.rssi, r.rssiMin,
               r.rssiMax, avg, securityName(r.enc), r.seenCount, Scanner::isPresent(r) ? "true" : "false",
               Scanner::isNew(r) ? "true" : "false");
        w.addf(PSTR("\"first_seen_ago\":%lu,\"last_seen_ago\":%lu,"), (unsigned long)(now - r.firstSeen),
               (unsigned long)(now - r.lastSeen));
        if (haveTime) {
            w.addf(PSTR("\"first_seen\":\"%s\",\"last_seen\":\"%s\"}"), first, last);
        } else {
            w.add("\"first_seen\":null,\"last_seen\":null}");
        }
    }
    w.add("]}");
    w.end();
}

// Detected counts from the latest scan plus the ESTIMATED load/congestion
// (see statistics.h) - never airtime utilization.
void handleChannels() {
    ChannelStat cs[Stats::kChannels];
    uint8_t scans = Stats::analyzeChannels(cs);

    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"scan_id\":%lu,\"basis\":\"estimated from detected APs\",\"scans_used\":%u,"
                "\"confidence\":\"%s\",\"recommended\":%u,\"least_populated\":%u,\"channels\":["),
           (unsigned long)Scanner::scanId(), scans, Stats::confidenceName(scans), Stats::recommendedChannel(cs),
           Stats::leastPopulatedChannel(cs));
    for (uint8_t c = 1; c <= Stats::kChannels; c++) {
        const ChannelStat& s = cs[c - 1];
        if (c == 14 && s.aps == 0) continue;  // Japan-only channel, list only if seen
        char a[8], b[8], m[8];
        w.addf(PSTR("%s{\"channel\":%u,\"frequency\":%u,\"aps\":%u,\"strong\":%u,\"medium\":%u,\"weak\":%u,"
                    "\"avg_rssi\":%s,\"max_rssi\":%s,\"min_rssi\":%s,\"overlapping\":%u,"),
               c == 1 ? "" : ",", c, channelToFrequency(c), s.aps, s.strong, s.medium, s.weak,
               rssiJson(s.avgRssi, a), rssiJson(s.maxRssi, b), rssiJson(s.minRssi, m), s.overlapping);
        w.addf(PSTR("\"load\":%.1f,\"congestion\":\"%s\"}"), s.load, Stats::levelName(s.level));
    }
    w.add("]}");
    w.end();
}

// GET /api/ble  ->  recently seen BLE devices, newest first
void handleBle() {
    uint32_t now = System::uptimeSeconds();
    uint32_t sid = Ble::scanId();
    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"available\":%s,\"scan_id\":%lu,\"devices\":["), Ble::available() ? "true" : "false",
           (unsigned long)sid);
    bool first = true;
    for (uint16_t i = 0; i < Ble::count(); i++) {
        const Ble::Device& d = Ble::at(i);
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X", d.addr[0], d.addr[1], d.addr[2], d.addr[3],
                 d.addr[4], d.addr[5]);
        const char* company = Ble::companyName(d.company);
        w.addf(PSTR("%s{\"address\":\"%s\",\"random\":%s,\"rssi\":%d,\"present\":%s,\"seen\":%u,"
                    "\"last_seen_ago\":%lu,\"company\":"),
               first ? "" : ",", addr, d.randomAddr ? "true" : "false", d.rssi, d.lastScanId == sid ? "true" : "false",
               d.seen, (unsigned long)(now - d.lastSeen));
        if (company) {
            w.addf(PSTR("\"%s\""), company);
        } else if (d.company != 0xFFFF) {
            w.addf(PSTR("\"0x%04X\""), d.company);
        } else {
            w.add("null");
        }
        w.buf() += F(",\"name\":");
        appendJsonString(w.buf(), d.name);
        w.add("}");
        first = false;
    }
    w.add("]}");
    w.end();
}

// GET /api/alerts  ->  newest first
void handleAlerts() {
    uint32_t ack = Alerts::bootInfo().ackId;
    uint32_t now = System::uptimeSeconds();
    uint16_t boot = (uint16_t)Alerts::bootInfo().boots;
    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"unread\":%u,\"last_id\":%lu,\"alerts\":["), Alerts::unread(), (unsigned long)Alerts::lastId());
    for (uint16_t k = Alerts::count(); k > 0; k--) {
        const Alert& a = Alerts::at(k - 1);
        w.addf(PSTR("%s{\"id\":%lu,\"type\":\"%s\",\"severity\":\"%s\",\"unread\":%s,\"message\":"),
               k == Alerts::count() ? "" : ",", (unsigned long)a.id, Alerts::typeName(a.type),
               Alerts::severityName(a.severity), a.id > ack ? "true" : "false");
        appendJsonString(w.buf(), Alerts::message(a, true).c_str());
        w.addf(PSTR(",\"epoch\":%lu,"), (unsigned long)a.epoch);
        if (a.boot == boot) {
            w.addf(PSTR("\"ago\":%lu,"), (unsigned long)(now - a.uptime));
        } else {
            w.add("\"ago\":null,");
        }
        if (a.type <= ALERT_STRONG_AP) {
            char b[18];
            formatBssid(a.bssid, b);
            w.buf() += F("\"ssid\":");
            appendJsonString(w.buf(), a.ssid);
            w.addf(PSTR(",\"bssid\":\"%s\",\"rssi\":%d,"), b, a.rssi);
        }
        w.addf(PSTR("\"channel\":%u,\"value\":%ld}"), a.channel, (long)a.value);
    }
    w.add("]}");
    w.end();
}

void handleAlertsAck() {
    if (!checkCsrf()) return;
    Alerts::ackAll();
    sendOk();
}

// Groups flash records into windows of `step` buckets so a long range stays
// under HISTORY_MAX_POINTS.
uint32_t historyWindow(uint32_t hours) {
    uint32_t buckets = hours * 3600 / HISTORY_BUCKET_S;
    uint32_t step = (buckets + HISTORY_MAX_POINTS - 1) / HISTORY_MAX_POINTS;
    return max<uint32_t>(1, step) * HISTORY_BUCKET_S;
}

uint32_t historyHoursArg() {
    long h = server.hasArg(F("hours")) ? server.arg(F("hours")).toInt() : config.historyHours;
    return (uint32_t)constrain(h, 1L, (long)config.historyHours);
}

// GET /api/history?hours=H  ->  [t, aps, open, hidden, strongest, avg_rssi] per window
void handleHistory() {
    uint32_t hours = historyHoursArg();
    uint32_t window = historyWindow(hours);
    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"persistent\":%s,\"hours\":%lu,\"window_s\":%lu,\"points\":["),
           History::persistent() ? "true" : "false", (unsigned long)hours, (unsigned long)window);
    if (History::persistent()) {
        struct {
            uint32_t key = 0, n = 0, aps = 0, open = 0, hidden = 0, rn = 0;
            int32_t strongest = 0, avg = 0;
            uint32_t ch[14] = {0};
        } acc;
        bool first = true;
        auto emit = [&]() {
            if (!acc.n) return;
            char a[8], b[8];
            int8_t st = acc.rn ? (int8_t)lroundf((float)acc.strongest / acc.rn) : RSSI_NONE;
            int8_t av = acc.rn ? (int8_t)lroundf((float)acc.avg / acc.rn) : RSSI_NONE;
            w.addf(PSTR("%s[%lu,%.1f,%.1f,%.1f,%s,%s,["), first ? "" : ",", (unsigned long)(acc.key * window),
                   (float)acc.aps / acc.n, (float)acc.open / acc.n, (float)acc.hidden / acc.n, rssiJson(st, a),
                   rssiJson(av, b));
            for (int c = 0; c < 14; c++) {  // APs per channel (heatmap)
                uint32_t tenths = (acc.ch[c] * 10 + acc.n / 2) / acc.n;
                if (tenths % 10) {
                    w.addf(PSTR("%s%lu.%lu"), c ? "," : "", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
                } else {
                    w.addf(PSTR("%s%lu"), c ? "," : "", (unsigned long)(tenths / 10));
                }
            }
            w.add("]]");
            first = false;
        };
        uint32_t from = (uint32_t)time(nullptr) - hours * 3600;
        History::forEachSummary(from, [&](const HistorySummary& h) {
            uint32_t key = h.t / window;
            if (acc.n && key != acc.key) {
                emit();
                acc = {};
            }
            acc.key = key;
            acc.n++;
            acc.aps += h.aps;
            acc.open += h.open;
            acc.hidden += h.hidden;
            for (int c = 0; c < 14; c++) acc.ch[c] += h.ch[c];
            if (h.strongest != RSSI_NONE) {
                acc.strongest += h.strongest;
                acc.avg += h.avgRssi;
                acc.rn++;
            }
        });
        emit();
    }
    w.add("]}");
    w.end();
}

// GET /api/history/live  ->  last scans from RAM:
// [seconds_ago, aps, open, hidden, strongest, avg_rssi, [aps on ch1..ch13(14)]]
void handleHistoryLive() {
    uint32_t id = Scanner::scanId();
    uint16_t n = History::liveCount();
    uint32_t now = System::uptimeSeconds();
    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"scan_id\":%lu,\"points\":["), (unsigned long)id);
    for (uint16_t k = n; k > 0; k--) {
        const HistorySummary& h = History::liveAt(id - k + 1);
        char a[8], b[8];
        w.addf(PSTR("%s[%lu,%u,%u,%u,%s,%s,["), k == n ? "" : ",", (unsigned long)(now - h.t), h.aps, h.open,
               h.hidden, rssiJson(h.strongest, a), rssiJson(h.avgRssi, b));
        for (int c = 0; c < 14; c++) w.addf(PSTR("%s%u"), c ? "," : "", h.ch[c]);
        w.add("]]");
    }
    w.add("]}");
    w.end();
}

// GET /api/history/ap?bssid=..&live=1         -> [seconds_ago, rssi|null] per scan (RAM)
// GET /api/history/ap?bssid=..&hours=H        -> [t, rssi, channel] per window (flash)
void handleHistoryAp() {
    uint8_t bssid[6];
    if (!parseBssid(server.arg(F("bssid")), bssid)) return sendError(400, F("bssid: AA:BB:CC:DD:EE:FF"));

    if (server.arg(F("live")) == "1") {
        const ApRecord* r = Scanner::find(bssid);
        if (!r) return sendError(404, F("AP not tracked"));
        uint32_t id = Scanner::scanId();
        uint16_t n = History::liveCount();
        uint32_t now = System::uptimeSeconds();
        ChunkWriter w(F("application/json"));
        w.add("{\"points\":[");
        for (uint16_t k = n; k > 0; k--) {
            uint32_t sid = id - k + 1;
            char a[8];
            w.addf(PSTR("%s[%lu,%s]"), k == n ? "" : ",", (unsigned long)(now - History::liveAt(sid).t),
                   rssiJson(r->live[sid % HISTORY_LIVE_POINTS], a));
        }
        w.add("]}");
        w.end();
        return;
    }

    uint32_t hours = historyHoursArg();
    uint32_t window = historyWindow(hours);
    ChunkWriter w(F("application/json"));
    w.addf(PSTR("{\"persistent\":%s,\"hours\":%lu,\"window_s\":%lu,\"points\":["),
           History::persistent() ? "true" : "false", (unsigned long)hours, (unsigned long)window);
    if (History::persistent()) {
        struct {
            uint32_t key = 0, n = 0;
            int32_t sum = 0;
            uint8_t ch = 0;
        } acc;
        bool first = true;
        auto emit = [&]() {
            if (!acc.n) return;
            w.addf(PSTR("%s[%lu,%d,%u]"), first ? "" : ",", (unsigned long)(acc.key * window),
                   (int)lroundf((float)acc.sum / acc.n), acc.ch);
            first = false;
        };
        uint32_t from = (uint32_t)time(nullptr) - hours * 3600;
        History::forEachApPoint(bssid, from, [&](uint32_t t, int8_t rssi, uint8_t ch) {
            uint32_t key = t / window;
            if (acc.n && key != acc.key) {
                emit();
                acc = {};
            }
            acc.key = key;
            acc.n++;
            acc.sum += rssi;
            acc.ch = ch;
        });
        emit();
    }
    w.add("]}");
    w.end();
}

void handleGetConfig() {
    JsonDocument d;
    d["device_name"] = config.deviceName;
    d["hostname"] = config.hostname;
    d["wifi_ssid"] = config.wifiSsid;
    d["wifi_password_set"] = config.wifiPass.length() > 0;
    d["timezone"] = config.timezone;
    d["scan_interval"] = config.scanInterval;
    d["auto_scan"] = config.autoScan;
    d["max_aps"] = config.maxAps;
    d["history_hours"] = config.historyHours;
    d["mqtt_enabled"] = config.mqttEnabled;
    d["mqtt_host"] = config.mqttHost;
    d["mqtt_port"] = config.mqttPort;
    d["mqtt_user"] = config.mqttUser;
    d["mqtt_password_set"] = config.mqttPass.length() > 0;
    d["mqtt_client_id"] = config.mqttClientId;
    d["mqtt_topic"] = config.mqttTopic;
    d["mqtt_topic_default"] = String(F("wifi-monitor/")) + config.hostname;
    d["mqtt_discovery"] = config.mqttDiscovery;
    d["mqtt_discovery_prefix"] = config.mqttDiscoveryPrefix;
    d["mqtt_publish_networks"] = config.mqttPublishNetworks;
    d["privacy_hide_ssid"] = config.privacyHideSsid;
    d["privacy_anon_bssid"] = config.privacyAnonBssid;
    d["alert_new_ap"] = config.alertNewAp;
    d["alert_open_ap"] = config.alertOpenAp;
    d["alert_strong_ap"] = config.alertStrongAp;
    d["alert_strong_rssi"] = config.alertStrongRssi;
    d["alert_density"] = config.alertDensity;
    d["alert_density_aps"] = config.alertDensityAps;
    d["alert_system"] = config.alertSystem;
    d["ip_static"] = config.ipStatic;
    d["ip_address"] = config.ipAddr;
    d["ip_gateway"] = config.ipGateway;
    d["ip_subnet"] = config.ipMask;
    d["ip_dns1"] = config.ipDns1;
    d["ip_dns2"] = config.ipDns2;
    d["scan_channel"] = config.scanChannel;
    d["ui_theme"] = config.uiTheme;
    d["ui_accent"] = config.uiAccent;
    d["ble_enabled"] = config.bleEnabled;
    d["ble_name"] = config.bleName;
    d["ble_bthome"] = config.bleBthome;
    d["ble_scan"] = config.bleScan;
    d["ble_scan_interval"] = config.bleScanInterval;
    JsonObject lim = d["limits"].to<JsonObject>();
    lim["min_scan_interval"] = MIN_SCAN_INTERVAL_S;
    lim["max_scan_interval"] = MAX_SCAN_INTERVAL_S;
    lim["min_aps"] = MIN_TRACKED_APS;
    lim["max_aps"] = MAX_TRACKED_APS_LIMIT;
    JsonArray hh = lim["history_hours"].to<JsonArray>();
    for (uint16_t h : Storage::kHistoryHours) hh.add(h);
    sendJson(200, d);
}

// ---- API: write ----------------------------------------------------------------

void handleScan() {
    if (!checkCsrf()) return;
    if (!Scanner::requestScan()) {
        sendError(429, F("Scan already running or requested too recently"));
        return;
    }
    JsonDocument d;
    d["ok"] = true;
    d["queued"] = true;
    d["allowed_now"] = WifiManager::scanAllowed();
    sendJson(202, d);
}

bool isPrintableAscii(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) return false;
    }
    return true;
}

bool parseBody(JsonDocument& body) {
    if (deserializeJson(body, server.arg(F("plain"))) || !body.is<JsonObject>()) {
        sendError(400, F("Body must be a JSON object"));
        return false;
    }
    return true;
}

// POST /api/wifi/connect {"ssid": "...", "password": "..."}
// Returns at once; the UI follows wifi.attempt_state in /api/status.
void handleWifiConnect() {
    if (!checkCsrf()) return;
    JsonDocument body;
    if (!parseBody(body)) return;
    String ssid = body["ssid"] | "";
    String pass = body["password"] | "";
    if (ssid.isEmpty() || ssid.length() > 32) return sendError(400, F("ssid: 1-32 bytes"));
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63))
        return sendError(400, F("password: empty (open network) or 8-63 characters"));
    WifiManager::connectTo(ssid, pass);
    sendOk();
}

// Partial update: only keys present in the body change. Everything is validated
// before anything is applied, so a bad field leaves the config untouched.
void handlePostConfig() {
    if (!checkCsrf()) return;
    JsonDocument body;
    if (!parseBody(body)) return;

    DeviceConfig next = config;
    bool rebootRequired = false;

    if (!body["device_name"].isNull()) {
        String v = body["device_name"].as<String>();
        v.trim();
        if (!body["device_name"].is<const char*>() || v.isEmpty() || v.length() > 32 || !isPrintableAscii(v))
            return sendError(400, F("device_name: 1-32 printable characters"));
        next.deviceName = v;
    }
    if (!body["hostname"].isNull()) {
        String v = body["hostname"].as<String>();
        v.trim();
        v.toLowerCase();
        if (!body["hostname"].is<const char*>() || !Storage::isValidHostname(v))
            return sendError(400, F("hostname: 1-31 chars a-z, 0-9, '-'"));
        rebootRequired |= v != config.hostname;
        next.hostname = v;
    }
    if (!body["wifi_ssid"].isNull()) {
        String v = body["wifi_ssid"].as<String>();
        if (!body["wifi_ssid"].is<const char*>() || v.length() > 32)
            return sendError(400, F("wifi_ssid: at most 32 bytes"));
        rebootRequired |= v != config.wifiSsid;
        next.wifiSsid = v;
    }
    if (!body["wifi_password"].isNull()) {
        String v = body["wifi_password"].as<String>();
        if (!body["wifi_password"].is<const char*>() || (v.length() > 0 && (v.length() < 8 || v.length() > 63)))
            return sendError(400, F("wifi_password: empty (open network) or 8-63 characters"));
        rebootRequired |= v != config.wifiPass;
        next.wifiPass = v;
    }
    if (!body["timezone"].isNull()) {
        String v = body["timezone"].as<String>();
        v.trim();
        if (!body["timezone"].is<const char*>() || v.isEmpty() || v.length() > 48 || !isPrintableAscii(v))
            return sendError(400, F("timezone: POSIX TZ string, 1-48 characters"));
        rebootRequired |= v != config.timezone;
        next.timezone = v;
    }
    if (!body["scan_interval"].isNull()) {
        if (!body["scan_interval"].is<int>()) return sendError(400, F("scan_interval: integer seconds"));
        int v = body["scan_interval"].as<int>();
        if (v < MIN_SCAN_INTERVAL_S || v > MAX_SCAN_INTERVAL_S)
            return sendError(400, F("scan_interval: out of range"));
        next.scanInterval = v;
    }
    if (!body["auto_scan"].isNull()) {
        if (!body["auto_scan"].is<bool>()) return sendError(400, F("auto_scan: boolean"));
        next.autoScan = body["auto_scan"].as<bool>();
    }
    if (!body["max_aps"].isNull()) {
        if (!body["max_aps"].is<int>()) return sendError(400, F("max_aps: integer"));
        int v = body["max_aps"].as<int>();
        if (v < MIN_TRACKED_APS || v > MAX_TRACKED_APS_LIMIT) return sendError(400, F("max_aps: out of range"));
        rebootRequired |= v != config.maxAps;
        next.maxAps = v;
    }
    if (!body["history_hours"].isNull()) {
        if (!body["history_hours"].is<int>() || !Storage::isValidHistoryHours(body["history_hours"].as<int>()))
            return sendError(400, F("history_hours: one of 1, 6, 12, 24, 72, 168"));
        next.historyHours = body["history_hours"].as<int>();
    }

    // ---- MQTT / privacy ----
    auto optString = [&](const char* key, String& dst, size_t maxLen, bool allowEmpty, bool (*valid)(const String&)) {
        if (body[key].isNull()) return true;
        if (!body[key].is<const char*>()) return false;
        String v = body[key].as<String>();
        v.trim();
        if (v.length() > maxLen || (!allowEmpty && v.isEmpty())) return false;
        if (v.length() && valid && !valid(v)) return false;
        dst = v;
        return true;
    };
    auto optBool = [&](const char* key, bool& dst) {
        if (body[key].isNull()) return true;
        if (!body[key].is<bool>()) return false;
        dst = body[key].as<bool>();
        return true;
    };
    auto noSpaces = [](const String& s) { return isPrintableAscii(s) && s.indexOf(' ') < 0; };
    auto clientIdOk = [](const String& s) {
        for (size_t i = 0; i < s.length(); i++) {
            if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return false;
        }
        return true;
    };
    if (!optBool("mqtt_enabled", next.mqttEnabled)) return sendError(400, F("mqtt_enabled: boolean"));
    if (!optString("mqtt_host", next.mqttHost, 64, true, noSpaces))
        return sendError(400, F("mqtt_host: host name or IP, max 64 characters"));
    if (!body["mqtt_port"].isNull()) {
        if (!body["mqtt_port"].is<int>() || body["mqtt_port"].as<int>() < 1 || body["mqtt_port"].as<int>() > 65535)
            return sendError(400, F("mqtt_port: 1-65535"));
        next.mqttPort = body["mqtt_port"].as<int>();
    }
    if (!optString("mqtt_user", next.mqttUser, 64, true, isPrintableAscii))
        return sendError(400, F("mqtt_user: max 64 printable characters"));
    if (!body["mqtt_password"].isNull()) {  // write-only; not trimmed
        String v = body["mqtt_password"].as<String>();
        if (!body["mqtt_password"].is<const char*>() || v.length() > 64 || !isPrintableAscii(v))
            return sendError(400, F("mqtt_password: max 64 printable characters"));
        next.mqttPass = v;
    }
    if (!optString("mqtt_client_id", next.mqttClientId, 32, true, clientIdOk))
        return sendError(400, F("mqtt_client_id: max 32 chars A-Z, a-z, 0-9, '-', '_'"));
    if (!optString("mqtt_topic", next.mqttTopic, 64, true, Storage::isValidTopic))
        return sendError(400, F("mqtt_topic: no spaces, '#', '+' or leading/trailing '/'"));
    if (!optString("mqtt_discovery_prefix", next.mqttDiscoveryPrefix, 64, false, Storage::isValidTopic))
        return sendError(400, F("mqtt_discovery_prefix: valid topic, e.g. homeassistant"));
    if (!optBool("mqtt_discovery", next.mqttDiscovery) || !optBool("mqtt_publish_networks", next.mqttPublishNetworks) ||
        !optBool("privacy_hide_ssid", next.privacyHideSsid) || !optBool("privacy_anon_bssid", next.privacyAnonBssid))
        return sendError(400, F("MQTT/privacy switches must be booleans"));
    if (next.mqttEnabled && next.mqttHost.isEmpty()) return sendError(400, F("mqtt_host: required when MQTT is enabled"));

    // ---- alerts ----
    if (!optBool("alert_new_ap", next.alertNewAp) || !optBool("alert_open_ap", next.alertOpenAp) ||
        !optBool("alert_strong_ap", next.alertStrongAp) || !optBool("alert_density", next.alertDensity) ||
        !optBool("alert_system", next.alertSystem))
        return sendError(400, F("alert switches must be booleans"));
    if (!body["alert_strong_rssi"].isNull()) {
        int v = body["alert_strong_rssi"] | 0;
        if (!body["alert_strong_rssi"].is<int>() || v < -70 || v > -20)
            return sendError(400, F("alert_strong_rssi: -70 to -20 dBm"));
        next.alertStrongRssi = v;
    }
    if (!body["alert_density_aps"].isNull()) {
        int v = body["alert_density_aps"] | 0;
        if (!body["alert_density_aps"].is<int>() || v < 2 || v > 50) return sendError(400, F("alert_density_aps: 2-50"));
        next.alertDensityAps = v;
    }

    // ---- network (static IP) ----
    if (!optBool("ip_static", next.ipStatic)) return sendError(400, F("ip_static: boolean"));
    if (!optString("ip_address", next.ipAddr, 15, true, Storage::isValidIp) ||
        !optString("ip_gateway", next.ipGateway, 15, true, Storage::isValidIp) ||
        !optString("ip_subnet", next.ipMask, 15, true, Storage::isValidIp) ||
        !optString("ip_dns1", next.ipDns1, 15, true, Storage::isValidIp) ||
        !optString("ip_dns2", next.ipDns2, 15, true, Storage::isValidIp))
        return sendError(400, F("IP settings: use dotted form, e.g. 192.168.1.50"));
    if (next.ipStatic && (next.ipAddr.isEmpty() || next.ipGateway.isEmpty() || next.ipMask.isEmpty()))
        return sendError(400, F("Static IP needs address, gateway and subnet mask"));
    bool ipChanged = next.ipStatic != config.ipStatic || next.ipAddr != config.ipAddr ||
                     next.ipGateway != config.ipGateway || next.ipMask != config.ipMask ||
                     next.ipDns1 != config.ipDns1 || next.ipDns2 != config.ipDns2;
    rebootRequired |= ipChanged;

    // ---- scanner channel / appearance ----
    if (!body["scan_channel"].isNull()) {
        int v = body["scan_channel"] | -1;
        if (!body["scan_channel"].is<int>() || v < 0 || v > 13) return sendError(400, F("scan_channel: 0 (all) or 1-13"));
        next.scanChannel = v;
    }
    auto themeOk = [](const String& s) { return s == "auto" || s == "light" || s == "dark"; };
    if (!optString("ui_theme", next.uiTheme, 5, false, themeOk)) return sendError(400, F("ui_theme: auto, light or dark"));
    if (!optString("ui_accent", next.uiAccent, 7, false, Storage::isValidColor))
        return sendError(400, F("ui_accent: color as #rrggbb"));

    // ---- BLE ----
    auto bleNameOk = [](const String& s) { return isPrintableAscii(s); };
    if (!optBool("ble_enabled", next.bleEnabled) || !optBool("ble_bthome", next.bleBthome) ||
        !optBool("ble_scan", next.bleScan))
        return sendError(400, F("BLE switches must be booleans"));
    if (!optString("ble_name", next.bleName, 20, false, bleNameOk)) return sendError(400, F("ble_name: 1-20 characters"));
    if (!body["ble_scan_interval"].isNull()) {
        int v = body["ble_scan_interval"] | 0;
        if (!body["ble_scan_interval"].is<int>() || v < MIN_BLE_SCAN_INTERVAL || v > 3600)
            return sendError(400, F("ble_scan_interval: 15-3600 s"));
        next.bleScanInterval = v;
    }
    // The BLE stack starts once; switching it on/off or renaming needs a restart.
    rebootRequired |= Ble::available() && (next.bleName != config.bleName || next.bleEnabled != config.bleEnabled);
    bool bleChanged = next.bleBthome != config.bleBthome || next.bleScan != config.bleScan ||
                      next.bleScanInterval != config.bleScanInterval;

    bool retentionChanged = next.historyHours != config.historyHours;
    bool mqttChanged = next.mqttEnabled != config.mqttEnabled || next.mqttHost != config.mqttHost ||
                       next.mqttPort != config.mqttPort || next.mqttUser != config.mqttUser ||
                       next.mqttPass != config.mqttPass || next.mqttClientId != config.mqttClientId ||
                       next.mqttTopic != config.mqttTopic || next.mqttDiscovery != config.mqttDiscovery ||
                       next.mqttDiscoveryPrefix != config.mqttDiscoveryPrefix || next.deviceName != config.deviceName;
    config = next;
    if (!Storage::saveConfig()) return sendError(500, F("Could not write config to flash"));
    if (retentionChanged) History::prune();
    if (mqttChanged) Mqtt::reconfigure();
    if (bleChanged) Ble::reconfigure();

    JsonDocument d;
    d["ok"] = true;
    d["reboot_required"] = rebootRequired;
    sendJson(200, d);
}

void handleReboot() {
    if (!checkCsrf()) return;
    sendOk();
    System::scheduleReboot(500);
}

void handleFactoryReset() {
    if (!checkCsrf()) return;
    if (!System::factoryReset()) return sendError(500, F("Could not remove config"));
    sendOk();
}

// ---- OTA: firmware ----------------------------------------------------------------
// POST /api/update/firmware (multipart "file"). The image must be an ESP image
// carrying our tag "ESPWM-FW|<version>|<board>|" for this board; otherwise the new
// image is never activated and the running firmware stays.

String s_otaError;
String s_otaTag;        // text after the tag prefix: "<version>|<board>|"
uint8_t s_otaMatch = 0; // chars of the prefix matched so far
bool s_otaCapture = false;

void scanTag(const uint8_t* p, size_t n) {
    static const char kPrefix[] = "ESPWM-FW|";
    for (size_t i = 0; i < n && s_otaTag.length() < 40; i++) {
        char c = (char)p[i];
        if (s_otaCapture) {
            if (c == 0) {  // end of the string in flash
                s_otaCapture = false;
                continue;
            }
            s_otaTag += c;
            continue;
        }
        if (!s_otaTag.isEmpty()) return;  // first tag found; ignore anything later
        s_otaMatch = (c == kPrefix[s_otaMatch]) ? s_otaMatch + 1 : (c == kPrefix[0] ? 1 : 0);
        if (s_otaMatch == sizeof(kPrefix) - 1) {
            s_otaCapture = true;
            s_otaMatch = 0;
        }
    }
}

void handleFirmwareUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        s_otaError = "";
        s_otaTag = "";
        s_otaMatch = 0;
        s_otaCapture = false;
        if (server.header(CSRF_HEADER) != CSRF_HEADER_VALUE) {
            s_otaError = F("Missing " CSRF_HEADER " header");
            return;
        }
        System::setBusy(true);
#if defined(ESP8266)
        uint32_t maxSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
#else
        uint32_t maxSize = UPDATE_SIZE_UNKNOWN;  // the next OTA partition
#endif
        LOGF("OTA: receiving firmware '%s' (max %lu bytes)", up.filename.c_str(), (unsigned long)maxSize);
        if (!Update.begin(maxSize, U_FLASH)) s_otaError = updateError();
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_otaError.length()) return;
        scanTag(up.buf, up.currentSize);
        if (Update.write(up.buf, up.currentSize) != up.currentSize) s_otaError = updateError();
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_otaError.length()) {
            Update.end(false);
            return;
        }
        int sep = s_otaTag.indexOf('|');
        String board = sep > 0 ? s_otaTag.substring(sep + 1, s_otaTag.indexOf('|', sep + 1)) : String();
        if (sep <= 0) {
            s_otaError = F("Not an ESP WiFi Monitor firmware (no version tag found)");
        } else if (board != BOARD_ID) {
            s_otaError = String(F("This firmware is for board '")) + board + F("', this device is '" BOARD_ID "'");
        } else if (!Update.end(true)) {
            s_otaError = updateError();
        } else {
            LOGF("OTA: firmware %s written (%lu bytes)", s_otaTag.substring(0, sep).c_str(),
                 (unsigned long)up.totalSize);
        }
        if (s_otaError.length()) Update.end(false);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.end(false);
        s_otaError = F("Upload aborted");
    }
}

void handleFirmwareDone() {
    if (s_otaError.isEmpty() && !Update.isFinished()) s_otaError = F("No firmware file received");
    if (s_otaError.length()) {
        LOGF("OTA: failed: %s", s_otaError.c_str());
        System::setBusy(false);
        return sendErrorText(400, s_otaError);
    }
    JsonDocument d;
    d["ok"] = true;
    d["version"] = s_otaTag.substring(0, s_otaTag.indexOf('|'));
    sendJson(200, d);
    System::scheduleReboot(1500);
}

// ---- OTA: web interface files ---------------------------------------------------------
// POST /api/update/webfile?path=/js/app.js (multipart "file"). Only web UI paths are
// writable, so config.json, history and alerts can never be touched this way.

File s_webFile;
String s_webPath, s_webError;
size_t s_webBytes = 0;

bool isWebName(const String& s, const char* ext) {
    if (!s.endsWith(ext) || s.length() <= strlen(ext) || s.length() > 40) return false;
    for (size_t i = 0; i < s.length() - strlen(ext); i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return s.indexOf("..") < 0;
}

bool isWebPath(const String& p) {
    if (p == F("/index.html") || p == F("/embed.html") || p == F("/favicon.ico")) return true;
    if (p.startsWith(F("/css/"))) return isWebName(p.substring(5), ".css");
    if (p.startsWith(F("/js/"))) return isWebName(p.substring(4), ".js");
    return false;
}

void handleWebFileUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        s_webError = "";
        s_webBytes = 0;
        s_webPath = server.arg(F("path"));
        if (server.header(CSRF_HEADER) != CSRF_HEADER_VALUE) {
            s_webError = F("Missing " CSRF_HEADER " header");
        } else if (!isWebPath(s_webPath)) {
            s_webError = String(F("Not a web interface path: ")) + s_webPath;
        } else {
            s_webFile = LittleFS.open(s_webPath + F(".tmp"), "w");
            if (!s_webFile) s_webError = F("Cannot create file");
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_webError.length()) return;
        s_webBytes += up.currentSize;
        if (s_webBytes > WEBFILE_MAX_BYTES) {
            s_webError = F("File too large");
        } else if (s_webFile.write(up.buf, up.currentSize) != up.currentSize) {
            s_webError = F("Write failed (file system full?)");
        }
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (s_webFile) s_webFile.close();
        if (up.status == UPLOAD_FILE_ABORTED && s_webError.isEmpty()) s_webError = F("Upload aborted");
        String tmp = s_webPath + F(".tmp");
        if (s_webError.length()) {
            if (isWebPath(s_webPath)) LittleFS.remove(tmp);
            return;
        }
        LittleFS.remove(s_webPath);
        if (!LittleFS.rename(tmp, s_webPath)) s_webError = F("Rename failed");
        else LOGF("OTA: web file %s updated (%u bytes)", s_webPath.c_str(), (unsigned)s_webBytes);
    }
}

void handleWebFileDone() {
    if (s_webError.isEmpty() && s_webBytes == 0) s_webError = F("No file received");
    if (s_webError.length()) return sendErrorText(400, s_webError);
    JsonDocument d;
    d["ok"] = true;
    d["path"] = s_webPath;
    d["bytes"] = s_webBytes;
    sendJson(200, d);
}

void handleNotFound() {
    if (server.uri().startsWith(F("/api/"))) {
        sendError(404, F("Unknown API endpoint"));
    } else if (captiveRedirectNeeded()) {
        redirectToSetup();
    } else {
        server.send(404, F("text/plain"), F("Not found"));
    }
}

}  // namespace

namespace WebUi {

void begin() {
#if defined(ESP8266)
    server.collectHeaders(CSRF_HEADER);
#else
    static const char* headers[] = {CSRF_HEADER};
    server.collectHeaders(headers, 1);
#endif

    for (const char* p : kPages) server.on(p, HTTP_GET, handleIndex);
    server.on(F("/embed"), HTTP_GET, handleEmbed);
    for (const char* p : kCaptiveProbes) server.on(p, HTTP_GET, redirectToSetup);
    server.serveStatic("/css", LittleFS, "/css", "max-age=300");
    server.serveStatic("/js", LittleFS, "/js", "max-age=300");
    server.on(F("/favicon.ico"), HTTP_GET, [] { server.send(204); });

    server.on(F("/api/status"), HTTP_GET, handleStatus);
    server.on(F("/api/networks"), HTTP_GET, handleNetworks);
    server.on(F("/api/channels"), HTTP_GET, handleChannels);
    server.on(F("/api/history"), HTTP_GET, handleHistory);
    server.on(F("/api/history/live"), HTTP_GET, handleHistoryLive);
    server.on(F("/api/history/ap"), HTTP_GET, handleHistoryAp);
    server.on(F("/api/alerts"), HTTP_GET, handleAlerts);
    server.on(F("/api/ble"), HTTP_GET, handleBle);
    server.on(F("/api/alerts/ack"), HTTP_POST, handleAlertsAck);
    server.on(F("/api/config"), HTTP_GET, handleGetConfig);
    server.on(F("/api/config"), HTTP_POST, handlePostConfig);
    server.on(F("/api/scan"), HTTP_POST, handleScan);
    server.on(F("/api/wifi/connect"), HTTP_POST, handleWifiConnect);
    server.on(F("/api/reboot"), HTTP_POST, handleReboot);
    server.on(F("/api/factory-reset"), HTTP_POST, handleFactoryReset);
    server.on(F("/api/update/firmware"), HTTP_POST, handleFirmwareDone, handleFirmwareUpload);
    server.on(F("/api/update/webfile"), HTTP_POST, handleWebFileDone, handleWebFileUpload);
    server.onNotFound(handleNotFound);

    server.begin();
    LOGF("HTTP server on port %d", HTTP_PORT);
}

void loop() { server.handleClient(); }

}  // namespace WebUi
