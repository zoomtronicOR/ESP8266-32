#pragma once
#include <Arduino.h>

// MQTT publisher + Home Assistant MQTT discovery.
//
// Topics under <base> (config.mqttTopic, default "wifi-monitor/<hostname>"):
//   availability  "online"/"offline" (retained, LWT)
//   status        {"value":"online","ip":..,"firmware":..,"free_heap":..,"timestamp":..}
//   uptime, rssi, ap_count, channel_count   {"value":N,"timestamp":..}
//   scan          summary of the latest scan
//   channels      {"aps":[13],"avg_rssi":[13],"max_rssi":[13],"timestamp":..}
//   networks      AP list (optional, privacy options applied)
//   new_ap        "ON"/"OFF" (retained)
//   alerts        {"event_type":"<type>","severity":..,"message":..,...} per alert
//   alert         "ON"/"OFF" (retained): unread warning/critical alerts exist
//   cmd           <- "scan" starts a scan, "ack_alerts" marks alerts as read (HA buttons)
namespace Mqtt {

void begin();
void loop();
void reconfigure();  // settings changed: drop the connection, reconnect with new ones
// Factory reset: remove this device from Home Assistant (empty retained discovery
// configs) and clear its retained topics on the broker, then disconnect.
void forgetDevice();

bool enabled();
bool connected();
const char* stateText();
uint32_t publishedCount();
String baseTopic();

}  // namespace Mqtt
