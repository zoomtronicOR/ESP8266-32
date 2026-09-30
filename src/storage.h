#pragma once
#include <Arduino.h>

struct DeviceConfig {
    String deviceName;
    String hostname;
    String wifiSsid;
    String wifiPass;   // never returned by the API or logged
    String timezone;   // POSIX TZ string
    uint16_t scanInterval;
    bool autoScan;
    uint16_t maxAps;
    uint16_t historyHours;  // one of Storage::kHistoryHours

    bool mqttEnabled;
    String mqttHost;
    uint16_t mqttPort;
    String mqttUser;
    String mqttPass;              // never returned by the API or logged
    String mqttClientId;          // empty = hostname
    String mqttTopic;             // base topic; empty = "wifi-monitor/<hostname>"
    bool mqttDiscovery;           // Home Assistant MQTT discovery
    String mqttDiscoveryPrefix;
    bool mqttPublishNetworks;     // publish the AP list on <base>/networks
    bool privacyHideSsid;         // applies to everything sent over MQTT
    bool privacyAnonBssid;        // BSSID -> "AP-XXXX" over MQTT

    bool alertNewAp;
    bool alertOpenAp;
    bool alertStrongAp;
    int8_t alertStrongRssi;       // newly seen AP at or above this is "very strong nearby"
    bool alertDensity;
    uint8_t alertDensityAps;      // APs on one channel
    bool alertSystem;             // WiFi/MQTT outages, low memory, abnormal restarts

    bool ipStatic;                // false = DHCP
    String ipAddr, ipGateway, ipMask, ipDns1, ipDns2;
    uint8_t scanChannel;          // 0 = all channels, 1..13 = only this one
    String uiTheme;               // default theme for browsers without their own choice: auto|light|dark
    String uiAccent;              // #rrggbb

    bool bleEnabled;              // only used on boards with a BLE radio
    String bleName;               // advertised name, 1-20 chars
    bool bleBthome;               // BTHome v2 advertising for Home Assistant
    bool bleScan;                 // periodic passive scan of nearby BLE devices
    uint16_t bleScanInterval;     // s
};

extern DeviceConfig config;

namespace Storage {

bool begin();
void loadConfig();
bool saveConfig();
bool factoryReset();  // removes config.json; web files and firmware stay

bool isValidHostname(const String& h);

constexpr uint16_t kHistoryHours[] = {1, 6, 12, 24, 72, 168};
bool isValidHistoryHours(int h);
// MQTT topic segment(s): printable, no spaces/wildcards, no leading/trailing '/'
bool isValidTopic(const String& t);
bool isValidIp(const String& s);
bool isValidColor(const String& s);  // #rrggbb

}  // namespace Storage
