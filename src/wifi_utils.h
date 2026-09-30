#pragma once
#include <Arduino.h>

// 2.4 GHz channel centre frequency in MHz, 0 for an unknown channel.
uint16_t channelToFrequency(uint8_t channel);

// Human-readable security for an ESP8266 ENC_TYPE_* value.
// The ESP8266 scan cannot distinguish WPA3; CCMP is reported as "WPA2".
const char* securityName(uint8_t encType);

void formatBssid(const uint8_t* bssid, char* out /* >= 18 bytes */);

// Appends `s` as a quoted, escaped JSON string. SSIDs are arbitrary bytes.
void appendJsonString(String& out, const char* s);
