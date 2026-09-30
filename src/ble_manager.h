#pragma once
#include <Arduino.h>

// Bluetooth LE on boards with a BLE radio (FEATURE_BLE_NATIVE, e.g. ESP32-C3), via NimBLE:
//  - BTHome v2 advertising: Home Assistant's Bluetooth integration discovers the
//    device by itself and shows the counts below as sensors (no pairing, no MQTT)
//  - a GATT service with a JSON summary (read/notify), e.g. for nRF Connect
//  - periodic passive scan of nearby BLE devices
// On boards without BLE every call is a no-op and available() is false.
namespace Ble {

struct Device {
    uint8_t addr[6];
    bool randomAddr;     // random/private addresses rotate: one phone can appear several times
    char name[21];
    int8_t rssi;
    uint16_t company;    // Bluetooth SIG company id from manufacturer data, 0xFFFF = none
    uint32_t lastSeen;   // System::uptimeSeconds()
    uint32_t lastScanId;
    uint16_t seen;
};

bool available();
void begin();
void loop();
void reconfigure();  // settings changed

bool enabled();
bool advertising();
bool scanning();     // the WiFi scanner waits while BLE scans (one radio)
String address();

uint16_t count();                // table of recently seen devices
const Device& at(uint16_t i);
uint32_t scanId();               // completed BLE scans
uint16_t lastScanDevices();      // devices in the latest scan
int8_t lastScanStrongest();      // RSSI_NONE-style INT8_MIN when none
uint32_t lastScanUptime();
const char* companyName(uint16_t id);

}  // namespace Ble
