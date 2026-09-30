#include "storage.h"

#include <ArduinoJson.h>
#include <IPAddress.h>
#include <LittleFS.h>

#include "config.h"
#include "system.h"

DeviceConfig config;

namespace {

void applyDefaults() {
    config.deviceName = DEFAULT_DEVICE_NAME;
    config.hostname = DEFAULT_HOSTNAME;
    config.wifiSsid = "";
    config.wifiPass = "";
    config.timezone = DEFAULT_TIMEZONE;
    config.scanInterval = DEFAULT_SCAN_INTERVAL_S;
    config.autoScan = true;
    config.maxAps = DEFAULT_MAX_APS;
    config.historyHours = DEFAULT_HISTORY_HOURS;
    config.mqttEnabled = false;
    config.mqttHost = "";
    config.mqttPort = DEFAULT_MQTT_PORT;
    config.mqttUser = "";
    config.mqttPass = "";
    config.mqttClientId = "";
    config.mqttTopic = "";
    config.mqttDiscovery = true;
    config.mqttDiscoveryPrefix = DEFAULT_DISCOVERY_PREFIX;
    config.mqttPublishNetworks = true;
    config.privacyHideSsid = false;
    config.privacyAnonBssid = false;
    config.alertNewAp = true;
    config.alertOpenAp = true;
    config.alertStrongAp = true;
    config.alertStrongRssi = DEFAULT_ALERT_STRONG_RSSI;
    config.alertDensity = true;
    config.alertDensityAps = DEFAULT_ALERT_DENSITY_APS;
    config.alertSystem = true;
    config.ipStatic = false;
    config.ipAddr = config.ipGateway = config.ipDns1 = config.ipDns2 = "";
    config.ipMask = "255.255.255.0";
    config.scanChannel = 0;
    config.uiTheme = "auto";
    config.uiAccent = DEFAULT_UI_ACCENT;
}

void sanitize() {
    config.scanInterval = constrain(config.scanInterval, MIN_SCAN_INTERVAL_S, MAX_SCAN_INTERVAL_S);
    config.maxAps = constrain(config.maxAps, MIN_TRACKED_APS, MAX_TRACKED_APS_LIMIT);
    if (!Storage::isValidHostname(config.hostname)) config.hostname = DEFAULT_HOSTNAME;
    if (config.deviceName.isEmpty()) config.deviceName = DEFAULT_DEVICE_NAME;
    if (config.timezone.isEmpty()) config.timezone = DEFAULT_TIMEZONE;
    if (!Storage::isValidHistoryHours(config.historyHours)) config.historyHours = DEFAULT_HISTORY_HOURS;
    if (config.mqttPort == 0) config.mqttPort = DEFAULT_MQTT_PORT;
    if (config.mqttTopic.length() && !Storage::isValidTopic(config.mqttTopic)) config.mqttTopic = "";
    if (!Storage::isValidTopic(config.mqttDiscoveryPrefix)) config.mqttDiscoveryPrefix = DEFAULT_DISCOVERY_PREFIX;
    config.alertStrongRssi = constrain(config.alertStrongRssi, -70, -20);
    config.alertDensityAps = constrain(config.alertDensityAps, 2, 50);
    if (config.scanChannel > 13) config.scanChannel = 0;
    if (config.uiTheme != "light" && config.uiTheme != "dark") config.uiTheme = "auto";
    if (!Storage::isValidColor(config.uiAccent)) config.uiAccent = DEFAULT_UI_ACCENT;
    // A static setup is only usable when complete; otherwise fall back to DHCP.
    if (config.ipStatic && !(Storage::isValidIp(config.ipAddr) && Storage::isValidIp(config.ipGateway) &&
                             Storage::isValidIp(config.ipMask))) {
        config.ipStatic = false;
    }
}

}  // namespace

namespace Storage {

bool begin() {
    // LittleFS on ESP8266 auto-formats an unformatted partition on first mount.
    if (!LittleFS.begin()) {
        LOGF("LittleFS mount failed");
        return false;
    }
    FSInfo info;
    LittleFS.info(info);
    LOGF("LittleFS: %u / %u bytes used", (unsigned)info.usedBytes, (unsigned)info.totalBytes);
    return true;
}

void loadConfig() {
    applyDefaults();
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        LOGF("No %s, using defaults", CONFIG_PATH);
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        LOGF("Config parse error (%s), using defaults", err.c_str());
        return;
    }
    config.deviceName = doc["device_name"] | DEFAULT_DEVICE_NAME;
    config.hostname = doc["hostname"] | DEFAULT_HOSTNAME;
    config.wifiSsid = doc["wifi_ssid"] | "";
    config.wifiPass = doc["wifi_password"] | "";
    config.timezone = doc["timezone"] | DEFAULT_TIMEZONE;
    config.scanInterval = doc["scan_interval"] | DEFAULT_SCAN_INTERVAL_S;
    config.autoScan = doc["auto_scan"] | true;
    config.maxAps = doc["max_aps"] | DEFAULT_MAX_APS;
    config.historyHours = doc["history_hours"] | DEFAULT_HISTORY_HOURS;
    config.mqttEnabled = doc["mqtt_enabled"] | false;
    config.mqttHost = doc["mqtt_host"] | "";
    config.mqttPort = doc["mqtt_port"] | DEFAULT_MQTT_PORT;
    config.mqttUser = doc["mqtt_user"] | "";
    config.mqttPass = doc["mqtt_password"] | "";
    config.mqttClientId = doc["mqtt_client_id"] | "";
    config.mqttTopic = doc["mqtt_topic"] | "";
    config.mqttDiscovery = doc["mqtt_discovery"] | true;
    config.mqttDiscoveryPrefix = doc["mqtt_discovery_prefix"] | DEFAULT_DISCOVERY_PREFIX;
    config.mqttPublishNetworks = doc["mqtt_publish_networks"] | true;
    config.privacyHideSsid = doc["privacy_hide_ssid"] | false;
    config.privacyAnonBssid = doc["privacy_anon_bssid"] | false;
    config.alertNewAp = doc["alert_new_ap"] | true;
    config.alertOpenAp = doc["alert_open_ap"] | true;
    config.alertStrongAp = doc["alert_strong_ap"] | true;
    config.alertStrongRssi = doc["alert_strong_rssi"] | DEFAULT_ALERT_STRONG_RSSI;
    config.alertDensity = doc["alert_density"] | true;
    config.alertDensityAps = doc["alert_density_aps"] | DEFAULT_ALERT_DENSITY_APS;
    config.alertSystem = doc["alert_system"] | true;
    config.ipStatic = doc["ip_static"] | false;
    config.ipAddr = doc["ip_address"] | "";
    config.ipGateway = doc["ip_gateway"] | "";
    config.ipMask = doc["ip_subnet"] | "255.255.255.0";
    config.ipDns1 = doc["ip_dns1"] | "";
    config.ipDns2 = doc["ip_dns2"] | "";
    config.scanChannel = doc["scan_channel"] | 0;
    config.uiTheme = doc["ui_theme"] | "auto";
    config.uiAccent = doc["ui_accent"] | DEFAULT_UI_ACCENT;
    sanitize();
    LOGF("Config loaded (device '%s')", config.deviceName.c_str());
}

bool saveConfig() {
    sanitize();
    JsonDocument doc;
    doc["device_name"] = config.deviceName;
    doc["hostname"] = config.hostname;
    doc["wifi_ssid"] = config.wifiSsid;
    doc["wifi_password"] = config.wifiPass;
    doc["timezone"] = config.timezone;
    doc["scan_interval"] = config.scanInterval;
    doc["auto_scan"] = config.autoScan;
    doc["max_aps"] = config.maxAps;
    doc["history_hours"] = config.historyHours;
    doc["mqtt_enabled"] = config.mqttEnabled;
    doc["mqtt_host"] = config.mqttHost;
    doc["mqtt_port"] = config.mqttPort;
    doc["mqtt_user"] = config.mqttUser;
    doc["mqtt_password"] = config.mqttPass;
    doc["mqtt_client_id"] = config.mqttClientId;
    doc["mqtt_topic"] = config.mqttTopic;
    doc["mqtt_discovery"] = config.mqttDiscovery;
    doc["mqtt_discovery_prefix"] = config.mqttDiscoveryPrefix;
    doc["mqtt_publish_networks"] = config.mqttPublishNetworks;
    doc["privacy_hide_ssid"] = config.privacyHideSsid;
    doc["privacy_anon_bssid"] = config.privacyAnonBssid;
    doc["alert_new_ap"] = config.alertNewAp;
    doc["alert_open_ap"] = config.alertOpenAp;
    doc["alert_strong_ap"] = config.alertStrongAp;
    doc["alert_strong_rssi"] = config.alertStrongRssi;
    doc["alert_density"] = config.alertDensity;
    doc["alert_density_aps"] = config.alertDensityAps;
    doc["alert_system"] = config.alertSystem;
    doc["ip_static"] = config.ipStatic;
    doc["ip_address"] = config.ipAddr;
    doc["ip_gateway"] = config.ipGateway;
    doc["ip_subnet"] = config.ipMask;
    doc["ip_dns1"] = config.ipDns1;
    doc["ip_dns2"] = config.ipDns2;
    doc["scan_channel"] = config.scanChannel;
    doc["ui_theme"] = config.uiTheme;
    doc["ui_accent"] = config.uiAccent;

    // Write to a temp file and rename so a power cut never leaves a half-written config.
    File f = LittleFS.open(CONFIG_TMP_PATH, "w");
    if (!f) return false;
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) return false;
    LittleFS.remove(CONFIG_PATH);
    bool ok = LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH);
    LOGF("Config saved: %s", ok ? "ok" : "FAILED");
    return ok;
}

bool factoryReset() {
    LittleFS.remove(CONFIG_TMP_PATH);
    bool ok = !LittleFS.exists(CONFIG_PATH) || LittleFS.remove(CONFIG_PATH);
    applyDefaults();
    LOGF("Factory reset: %s", ok ? "ok" : "FAILED");
    return ok;
}

bool isValidHistoryHours(int h) {
    for (uint16_t v : kHistoryHours) {
        if (v == h) return true;
    }
    return false;
}

bool isValidTopic(const String& t) {
    if (t.isEmpty() || t.length() > 64 || t[0] == '/' || t[t.length() - 1] == '/') return false;
    for (size_t i = 0; i < t.length(); i++) {
        char c = t[i];
        if (c <= 0x20 || c > 0x7e || c == '#' || c == '+') return false;
    }
    return true;
}

bool isValidIp(const String& s) {
    IPAddress ip;
    return s.length() >= 7 && ip.fromString(s);
}

bool isValidColor(const String& s) {
    if (s.length() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

bool isValidHostname(const String& h) {
    if (h.length() < 1 || h.length() > 31) return false;
    if (h[0] == '-' || h[h.length() - 1] == '-') return false;
    for (size_t i = 0; i < h.length(); i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

}  // namespace Storage
