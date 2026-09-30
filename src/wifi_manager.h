#pragma once
#include <Arduino.h>

// Station connection with a setup hotspot (captive portal):
//  - no credentials         -> hotspot only (192.168.4.1), every DNS name resolves to it
//  - STA down for too long  -> hotspot + STA, retrying STA while no one uses the hotspot
//  - STA up                 -> hotspot switched off once idle (or after AP_FORCE_SHUTDOWN_MS)
namespace WifiManager {

void begin();
void loop();

// Tries a network picked in the web UI. Credentials are saved to config only
// after the connection succeeds; on failure the previous config stays in effect.
void connectTo(const String& ssid, const String& password);

bool staConnected();
bool apActive();
const String& apSsid();
const char* modeName();
// Outcome of the last connectTo(): "", "connecting", "connected",
// "wrong_password", "no_ssid" or "failed".
const char* attemptState();
const String& attemptSsid();

// False while a STA connection attempt is in flight: a scan hops channels
// and would break the association/DHCP exchange.
bool scanAllowed();

}  // namespace WifiManager
