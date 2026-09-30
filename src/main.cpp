#include <Arduino.h>

#include "alerts.h"
#include "ble_manager.h"
#include "config.h"
#include "console.h"
#include "history.h"
#include "mqtt_manager.h"
#include "oui.h"
#include "storage.h"
#include "system.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "wifi_scanner.h"

void setup() {
    Serial.begin(115200);
    Serial.println();
    LOGF("%s %s", FW_NAME, FW_VERSION);

    System::begin();
    Storage::begin();
    Storage::loadConfig();
    Oui::begin();
    WifiManager::begin();
    Alerts::begin();
    Scanner::begin(config.maxAps);
    History::begin();
    Mqtt::begin();
    WebUi::begin();
    Ble::begin();
    LOGF("Boot done, free heap %u bytes", (unsigned)ESP.getFreeHeap());
}

// Everything is cooperative and non-blocking: the scan runs asynchronously in
// the SDK and each module only polls its state, so the watchdog is always fed.
void loop() {
    System::loop();
    WifiManager::loop();
    // one radio: the WiFi scan waits while BLE scans (and BLE waits for WiFi scans)
    Scanner::loop(WifiManager::scanAllowed() && !System::busy() && !Ble::scanning());
    History::loop();
    Mqtt::loop();
    Alerts::loop();
    Ble::loop();
    WebUi::loop();
    Console::loop();
}
