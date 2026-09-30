#include "mqtt_manager.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "alerts.h"
#include "config.h"
#include "storage.h"
#include "system.h"
#include "statistics.h"
#include "wifi_manager.h"
#include "wifi_scanner.h"
#include "wifi_utils.h"

namespace {

WiFiClient s_net;
PubSubClient s_mqtt(s_net);

// Copies: PubSubClient keeps the host pointer, and config strings can be replaced.
String s_host, s_base, s_clientId;
const char* s_state = "disabled";
uint32_t s_nextAttemptMs = 0;
uint32_t s_backoffMs = MQTT_RETRY_MIN_MS;
uint32_t s_published = 0;
uint32_t s_lastScanPublished = 0;
uint32_t s_lastStateMs = 0;
bool s_discoveryPending = false;
bool s_wasConnected = false;
uint32_t s_lastAlertPublished = 0;
int8_t s_alertState = -1;  // last published "alert" binary state, -1 = unknown

String nodeId() { return String(F("wifimon_")) + String(ESP.getChipId(), HEX); }

String timestampJson() {
    char iso[24];
    if (!System::formatIsoTime(System::uptimeSeconds(), iso, sizeof(iso))) return F("null");
    return String('"') + iso + '"';
}

// Streams the payload, so its size is not limited by the PubSubClient buffer.
bool pub(const String& topic, const String& payload, bool retained = false) {
    if (!s_mqtt.connected()) return false;
    bool ok = s_mqtt.beginPublish(topic.c_str(), payload.length(), retained) &&
              s_mqtt.print(payload) == payload.length() && s_mqtt.endPublish();
    if (ok) s_published++;
    return ok;
}

bool pubValue(const char* suffix, const String& valueJson) {
    return pub(s_base + '/' + suffix,
               String(F("{\"value\":")) + valueJson + F(",\"timestamp\":") + timestampJson() + '}');
}

// "AP-XXXX": stable per BSSID and device, not reversible to the MAC.
String anonBssid(const uint8_t* bssid) {
    uint32_t h = 2166136261u;
    uint32_t salt = ESP.getChipId();
    for (int i = 0; i < 4; i++) h = (h ^ ((salt >> (8 * i)) & 0xff)) * 16777619u;
    for (int i = 0; i < 6; i++) h = (h ^ bssid[i]) * 16777619u;
    char out[8];
    snprintf(out, sizeof(out), "AP-%04X", (unsigned)((h ^ (h >> 16)) & 0xffff));
    return out;
}

// SSID/BSSID fields for one AP with the privacy options applied.
void appendIdentity(String& s, const ApRecord& r) {
    if (!config.privacyHideSsid) {
        s += F("\"ssid\":");
        appendJsonString(s, r.ssid);
        s += ',';
    }
    s += F("\"bssid\":\"");
    if (config.privacyAnonBssid) {
        s += anonBssid(r.bssid);
    } else {
        char b[18];
        formatBssid(r.bssid, b);
        s += b;
    }
    s += F("\",");
}

// ---- Home Assistant discovery --------------------------------------------------

struct Entity {
    const char* component;
    const char* id;       // unique per device; also the discovery topic node
    const char* name;     // HA shows "<device name> <name>"
    const char* topic;    // state topic suffix
    const char* tmpl;     // value_template
    const char* unit;
    const char* devClass;
    const char* stateClass;
    const char* icon;
    bool diagnostic;
};

const Entity kEntities[] = {
    {"sensor", "ap_count", "AP count", "ap_count", "{{ value_json.value }}", "APs", nullptr, "measurement", "mdi:access-point-network", false},
    {"sensor", "strongest_signal", "Strongest signal", "scan", "{{ value_json.strongest }}", "dBm", "signal_strength", "measurement", nullptr, false},
    {"sensor", "average_signal", "Average signal", "scan", "{{ value_json.average }}", "dBm", "signal_strength", "measurement", nullptr, false},
    {"sensor", "open_networks", "Open networks", "scan", "{{ value_json.open }}", "APs", nullptr, "measurement", "mdi:wifi-lock-open", false},
    {"sensor", "hidden_networks", "Hidden networks", "scan", "{{ value_json.hidden }}", "APs", nullptr, "measurement", "mdi:wifi-strength-off-outline", false},
    {"sensor", "channels_used", "Channels used", "channel_count", "{{ value_json.value }}", nullptr, nullptr, "measurement", "mdi:format-list-numbered", false},
    {"sensor", "channel_1", "Channel 1", "channels", "{{ value_json.aps[0] }}", "APs", nullptr, "measurement", "mdi:numeric-1-box", false},
    {"sensor", "channel_6", "Channel 6", "channels", "{{ value_json.aps[5] }}", "APs", nullptr, "measurement", "mdi:numeric-6-box", false},
    {"sensor", "channel_11", "Channel 11", "channels", "{{ value_json.aps[10] }}", "APs", nullptr, "measurement", "mdi:numeric-1-box-multiple", false},
    {"sensor", "scan_count", "Scan count", "scan", "{{ value_json.scan_id }}", nullptr, nullptr, "total_increasing", "mdi:counter", true},
    {"sensor", "wifi_rssi", "WiFi RSSI", "rssi", "{{ value_json.value }}", "dBm", "signal_strength", "measurement", nullptr, true},
    {"sensor", "uptime", "Uptime", "uptime", "{{ value_json.value }}", "s", "duration", "total_increasing", nullptr, true},
    {"sensor", "free_heap", "Free heap", "status", "{{ value_json.free_heap }}", "B", "data_size", "measurement", "mdi:memory", true},
    {"sensor", "recommended_channel", "Recommended channel", "channels", "{{ value_json.recommended }}", nullptr, nullptr, nullptr, "mdi:wifi-star", false},
    {"sensor", "congestion_1", "Congestion channel 1", "channels", "{{ value_json.congestion[0] }}", nullptr, nullptr, nullptr, "mdi:gauge", false},
    {"sensor", "congestion_6", "Congestion channel 6", "channels", "{{ value_json.congestion[5] }}", nullptr, nullptr, nullptr, "mdi:gauge", false},
    {"sensor", "congestion_11", "Congestion channel 11", "channels", "{{ value_json.congestion[10] }}", nullptr, nullptr, nullptr, "mdi:gauge", false},
    {"binary_sensor", "new_ap", "New AP", "new_ap", nullptr, nullptr, nullptr, nullptr, "mdi:access-point-plus", false},
    {"binary_sensor", "alert", "Alert", "alert", nullptr, nullptr, "problem", nullptr, nullptr, false},
};

struct Button {
    const char* id;
    const char* name;
    const char* payload;
    const char* icon;
};
const Button kButtons[] = {
    {"scan", "Scan now", "scan", "mdi:wifi-refresh"},
    {"clear_alerts", "Mark alerts as read", "ack_alerts", "mdi:bell-check"},
};
// Discovery ids used by older firmware; cleared so HA does not keep dead entities.
const char* const kRetiredEvents[] = {"new_access_point"};

void addDevice(JsonDocument& doc) {
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"].to<JsonArray>().add(nodeId());
    dev["name"] = config.deviceName;
    dev["model"] = String(F(FW_NAME " (" BOARD_NAME ")"));
    dev["manufacturer"] = "DIY";
    dev["sw_version"] = FW_VERSION;
    if (WifiManager::staConnected()) dev["configuration_url"] = String(F("http://")) + WiFi.localIP().toString() + '/';
}

String discoveryTopic(const char* component, const char* id) {
    return config.mqttDiscoveryPrefix + '/' + component + '/' + nodeId() + '/' + id + F("/config");
}

bool publishDiscoveryDoc(const char* component, const char* id, JsonDocument& doc) {
    doc["unique_id"] = nodeId() + '_' + id;
    doc["availability_topic"] = s_base + F("/availability");
    addDevice(doc);
    String payload;
    serializeJson(doc, payload);
    return pub(discoveryTopic(component, id), payload, true);
}

void publishDiscovery() {
    s_discoveryPending = false;
    if (!config.mqttDiscovery) return;
    uint8_t ok = 0, total = 0;
    for (const Entity& e : kEntities) {
        JsonDocument doc;
        doc["name"] = e.name;
        doc["state_topic"] = s_base + '/' + e.topic;
        if (e.tmpl) doc["value_template"] = e.tmpl;
        if (e.unit) doc["unit_of_measurement"] = e.unit;
        if (e.devClass) doc["device_class"] = e.devClass;
        if (e.stateClass) doc["state_class"] = e.stateClass;
        if (e.icon) doc["icon"] = e.icon;
        if (e.diagnostic) doc["entity_category"] = "diagnostic";
        total++;
        ok += publishDiscoveryDoc(e.component, e.id, doc);
        yield();
    }
    for (const Button& b : kButtons) {  // HA buttons -> <base>/cmd
        JsonDocument doc;
        doc["name"] = b.name;
        doc["command_topic"] = s_base + F("/cmd");
        doc["payload_press"] = b.payload;
        doc["icon"] = b.icon;
        total++;
        ok += publishDiscoveryDoc("button", b.id, doc);
    }
    {
        JsonDocument doc;  // HA event entity fed by <base>/alerts; one event type per alert type
        doc["name"] = "Alert";
        doc["state_topic"] = s_base + F("/alerts");
        JsonArray types = doc["event_types"].to<JsonArray>();
        for (uint8_t t = 0; t < ALERT_TYPE_COUNT; t++) types.add(Alerts::typeName(t));
        doc["icon"] = "mdi:bell-alert";
        total++;
        ok += publishDiscoveryDoc("event", "alert", doc);
    }
    for (const char* id : kRetiredEvents) pub(discoveryTopic("event", id), "", true);
    LOGF("MQTT: discovery published (%u/%u entities)", ok, total);
}

// ---- State publishing ----------------------------------------------------------

void publishState() {
    s_lastStateMs = millis();
    String st = String(F("{\"value\":\"online\",\"ip\":\"")) + WiFi.localIP().toString() + F("\",\"firmware\":\"") +
                FW_VERSION + F("\",\"free_heap\":") + ESP.getFreeHeap() + F(",\"timestamp\":") + timestampJson() + '}';
    pub(s_base + F("/status"), st);
    pubValue("uptime", String(System::uptimeSeconds()));
    pubValue("rssi", String(WiFi.RSSI()));
}

// Two passes over the same data: first to measure, then to stream, because
// MQTT needs the payload length up front and the list may not fit in RAM.
template <typename Sink>
void networksJson(const String& ts, Sink sink) {
    sink(String(F("{\"timestamp\":")) + ts + F(",\"networks\":["));
    const ApRecord* recs = Scanner::records();
    bool first = true;
    for (uint16_t i = 0; i < Scanner::count(); i++) {
        const ApRecord& r = recs[i];
        if (!Scanner::isPresent(r)) continue;
        String s = first ? "{" : ",{";
        first = false;
        appendIdentity(s, r);
        s += F("\"channel\":");
        s += r.channel;
        s += F(",\"rssi\":");
        s += r.rssi;
        s += F(",\"security\":\"");
        s += securityName(r.enc);
        s += F("\"}");
        sink(s);
    }
    sink(F("]}"));
}

void publishNetworks() {
    String ts = timestampJson();
    size_t len = 0;
    networksJson(ts, [&](const String& s) { len += s.length(); });
    String topic = s_base + F("/networks");
    if (!s_mqtt.beginPublish(topic.c_str(), len, false)) return;
    networksJson(ts, [&](const String& s) { s_mqtt.print(s); });
    if (s_mqtt.endPublish()) s_published++;
}

// Every alert with an id above the last one sent, oldest first. Alerts raised
// while offline (e.g. the WiFi outage itself) go out after reconnecting.
void publishAlerts() {
    for (uint16_t i = 0; i < Alerts::count(); i++) {
        const Alert& a = Alerts::at(i);
        if (a.id <= s_lastAlertPublished) continue;
        String s = F("{\"event_type\":\"");
        s += Alerts::typeName(a.type);
        s += F("\",\"type\":\"");
        s += Alerts::typeName(a.type);
        s += F("\",\"severity\":\"");
        s += Alerts::severityName(a.severity);
        s += F("\",\"message\":");
        appendJsonString(s, Alerts::message(a, !config.privacyHideSsid && !config.privacyAnonBssid).c_str());
        if (a.type <= ALERT_STRONG_AP) {  // AP related
            s += ',';
            ApRecord tmp = {};
            memcpy(tmp.bssid, a.bssid, 6);
            strlcpy(tmp.ssid, a.ssid, sizeof(tmp.ssid));
            appendIdentity(s, tmp);
            s += F("\"rssi\":");
            s += a.rssi;
        }
        if (a.channel) {
            s += F(",\"channel\":");
            s += a.channel;
        }
        s += F(",\"value\":");
        s += a.value;
        s += F(",\"timestamp\":");
        if (a.epoch) {
            char iso[24];
            time_t t = a.epoch;
            struct tm tmLocal;
            localtime_r(&t, &tmLocal);
            strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tmLocal);
            s += '"';
            s += iso;
            s += '"';
        } else {
            s += F("null");
        }
        s += '}';
        if (!pub(s_base + F("/alerts"), s)) return;  // retry on the next loop
        s_lastAlertPublished = a.id;
    }
    int8_t state = Alerts::unreadProblem() ? 1 : 0;
    if (state != s_alertState && pub(s_base + F("/alert"), state ? F("ON") : F("OFF"), true)) s_alertState = state;
}

void publishScan() {
    uint32_t id = Scanner::scanId();
    s_lastScanPublished = id;
    ScanSummary sum = Scanner::summary();

    uint8_t aps[13] = {0};
    int32_t rsum[13] = {0};
    int8_t rmax[13];
    memset(rmax, RSSI_NONE, sizeof(rmax));
    int32_t total = 0;
    const ApRecord* recs = Scanner::records();
    for (uint16_t i = 0; i < Scanner::count(); i++) {
        const ApRecord& r = recs[i];
        if (!Scanner::isPresent(r)) continue;
        total += r.rssi;
        if (r.channel < 1 || r.channel > 13) continue;
        uint8_t c = r.channel - 1;
        aps[c]++;
        rsum[c] += r.rssi;
        if (r.rssi > rmax[c]) rmax[c] = r.rssi;
    }

    String ts = timestampJson();
    pubValue("ap_count", String(sum.present));
    pubValue("channel_count", String(sum.channelsUsed));

    String s = F("{\"scan_id\":");
    s += id;
    s += F(",\"duration_ms\":");
    s += Scanner::lastScanDurationMs();
    s += F(",\"detected\":");
    s += sum.present;
    s += F(",\"open\":");
    s += sum.open;
    s += F(",\"hidden\":");
    s += sum.hidden;
    s += F(",\"strong\":");
    s += sum.strong;
    s += F(",\"new\":");
    s += sum.isNew;
    s += F(",\"strongest\":");
    s += sum.present ? String(sum.strongest) : String(F("null"));
    s += F(",\"average\":");
    s += sum.present ? String((int)lroundf((float)total / sum.present)) : String(F("null"));
    s += F(",\"timestamp\":");
    s += ts;
    s += '}';
    pub(s_base + F("/scan"), s);

    ChannelStat cs[Stats::kChannels];
    uint8_t scans = Stats::analyzeChannels(cs);
    s = F("{\"aps\":[");
    for (int c = 0; c < 13; c++) {
        if (c) s += ',';
        s += aps[c];
    }
    // Estimated from detected APs, overlap and signal over recent scans; not airtime.
    s += F("],\"load\":[");
    for (int c = 0; c < 13; c++) {
        if (c) s += ',';
        s += String(cs[c].load, 1);
    }
    s += F("],\"congestion\":[");
    for (int c = 0; c < 13; c++) {
        if (c) s += ',';
        s += '"';
        s += Stats::levelName(cs[c].level);
        s += '"';
    }
    s += F("],\"confidence\":\"");
    s += Stats::confidenceName(scans);
    s += F("\",\"recommended\":");
    s += Stats::recommendedChannel(cs);
    s += F(",\"least_populated\":");
    s += Stats::leastPopulatedChannel(cs);
    s += F("],\"avg_rssi\":[");
    for (int c = 0; c < 13; c++) {
        if (c) s += ',';
        s += aps[c] ? String((int)lroundf((float)rsum[c] / aps[c])) : String(F("null"));
    }
    s += F("],\"max_rssi\":[");
    for (int c = 0; c < 13; c++) {
        if (c) s += ',';
        s += aps[c] ? String(rmax[c]) : String(F("null"));
    }
    s += F("],\"timestamp\":");
    s += ts;
    s += '}';
    pub(s_base + F("/channels"), s);

    pub(s_base + F("/new_ap"), sum.isNew ? F("ON") : F("OFF"), true);
    if (config.mqttPublishNetworks) publishNetworks();
}

// ---- Connection ------------------------------------------------------------------

void onMessage(char* topic, byte* payload, unsigned int len) {
    String t(topic);
    String p;
    p.concat((const char*)payload, len);
    if (t == s_base + F("/cmd")) {
        if (p == F("scan")) {
            LOGF("MQTT: scan requested");
            Scanner::requestScan();
        } else if (p == F("ack_alerts")) {
            LOGF("MQTT: alerts marked as read");
            Alerts::ackAll();
        }
    } else if (t == config.mqttDiscoveryPrefix + F("/status") && p == F("online")) {
        s_discoveryPending = true;  // HA restarted: announce entities again (from loop, not here)
    }
}

const char* describe(int state) {
    switch (state) {
        case MQTT_CONNECTION_TIMEOUT:      return "connection timeout";
        case MQTT_CONNECTION_LOST:         return "connection lost";
        case MQTT_CONNECT_FAILED:          return "broker unreachable";
        case MQTT_DISCONNECTED:            return "disconnected";
        case MQTT_CONNECTED:               return "connected";
        case MQTT_CONNECT_BAD_PROTOCOL:    return "bad protocol";
        case MQTT_CONNECT_BAD_CLIENT_ID:   return "client ID rejected";
        case MQTT_CONNECT_UNAVAILABLE:     return "broker unavailable";
        case MQTT_CONNECT_BAD_CREDENTIALS: return "bad username/password";
        case MQTT_CONNECT_UNAUTHORIZED:    return "not authorized";
        default:                           return "error";
    }
}

bool connect() {
    s_host = config.mqttHost;
    s_clientId = config.mqttClientId.length() ? config.mqttClientId : config.hostname;
    s_net.setTimeout(MQTT_SOCKET_TIMEOUT_MS);
    s_mqtt.setServer(s_host.c_str(), config.mqttPort);
    s_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    s_mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_MS / 1000);
    s_mqtt.setCallback(onMessage);

    String will = s_base + F("/availability");
    bool ok = config.mqttUser.length()
                  ? s_mqtt.connect(s_clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(), will.c_str(),
                                   0, true, "offline")
                  : s_mqtt.connect(s_clientId.c_str(), nullptr, nullptr, will.c_str(), 0, true, "offline");
    s_state = describe(s_mqtt.state());
    if (!ok) {
        LOGF("MQTT: connect to %s:%u failed (%s), retry in %lus", s_host.c_str(), config.mqttPort, s_state,
             (unsigned long)(s_backoffMs / 1000));
        return false;
    }
    LOGF("MQTT: connected to %s:%u as '%s', base topic '%s'", s_host.c_str(), config.mqttPort, s_clientId.c_str(),
         s_base.c_str());
    pub(will, F("online"), true);
    s_mqtt.subscribe((s_base + F("/cmd")).c_str());
    if (config.mqttDiscovery) s_mqtt.subscribe((config.mqttDiscoveryPrefix + F("/status")).c_str());
    s_discoveryPending = true;
    s_lastScanPublished = 0;  // republish the latest scan right away
    s_lastStateMs = 0;
    s_alertState = -1;
    return true;
}

}  // namespace

namespace Mqtt {

void begin() {
    // Alerts from earlier boots were sent back then; send only this boot's (e.g. a watchdog reset).
    for (uint16_t i = 0; i < Alerts::count(); i++) {
        const Alert& a = Alerts::at(i);
        if (a.boot != Alerts::bootInfo().boots && a.id > s_lastAlertPublished) s_lastAlertPublished = a.id;
    }
    reconfigure();
}

void reconfigure() {
    if (s_mqtt.connected()) {
        pub(s_base + F("/availability"), F("offline"), true);
        s_mqtt.disconnect();
    }
    s_base = baseTopic();
    s_nextAttemptMs = millis();
    s_backoffMs = MQTT_RETRY_MIN_MS;
}

void loop() {
    if (!enabled()) {
        if (s_mqtt.connected()) s_mqtt.disconnect();
        s_state = config.mqttEnabled ? "no broker configured" : "disabled";
        return;
    }
    if (!WifiManager::staConnected()) {
        s_state = "waiting for WiFi";
        return;
    }
    if (!s_mqtt.connected()) {
        if (s_wasConnected) {
            s_wasConnected = false;
            s_state = describe(s_mqtt.state());
            LOGF("MQTT: %s", s_state);
        }
        if ((int32_t)(millis() - s_nextAttemptMs) < 0) return;
        if (connect()) {
            s_wasConnected = true;
            s_backoffMs = MQTT_RETRY_MIN_MS;
        } else {
            s_nextAttemptMs = millis() + s_backoffMs;
            s_backoffMs = min<uint32_t>(s_backoffMs * 2, MQTT_RETRY_MAX_MS);
        }
        return;
    }
    s_mqtt.loop();
    if (s_discoveryPending) publishDiscovery();
    if (Scanner::scanId() > 0 && Scanner::scanId() != s_lastScanPublished) publishScan();
    publishAlerts();
    if (s_lastStateMs == 0 || millis() - s_lastStateMs >= MQTT_STATE_INTERVAL_MS) publishState();
}

void forgetDevice() {
    if (!s_mqtt.connected()) return;
    // An empty retained config deletes the entity in HA; the device goes with its last entity.
    for (const Entity& e : kEntities) pub(discoveryTopic(e.component, e.id), "", true);
    for (const Button& b : kButtons) pub(discoveryTopic("button", b.id), "", true);
    pub(discoveryTopic("event", "alert"), "", true);
    for (const char* id : kRetiredEvents) pub(discoveryTopic("event", id), "", true);
    pub(s_base + F("/new_ap"), "", true);
    pub(s_base + F("/alert"), "", true);
    pub(s_base + F("/availability"), "", true);
    s_mqtt.disconnect();  // clean disconnect: the broker does not publish the will
    LOGF("MQTT: device removed from Home Assistant");
}

bool enabled() { return config.mqttEnabled && config.mqttHost.length() > 0; }
bool connected() { return s_mqtt.connected(); }
const char* stateText() { return s_state; }
uint32_t publishedCount() { return s_published; }

String baseTopic() {
    return config.mqttTopic.length() ? config.mqttTopic : String(F("wifi-monitor/")) + config.hostname;
}

}  // namespace Mqtt
