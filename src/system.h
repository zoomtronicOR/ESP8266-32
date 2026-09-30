#pragma once
#include <Arduino.h>

#define LOGF(fmt, ...) \
    Serial.printf_P(PSTR("[%7lu] " fmt "\n"), (unsigned long)(millis() / 1000), ##__VA_ARGS__)

namespace System {

void begin();
void loop();

// Seconds since boot; survives the 49-day millis() rollover as long as it is
// called at least once per rollover period (loop() does).
uint32_t uptimeSeconds();

bool timeSynced();
// Formats a moment given as uptime seconds into local ISO-8601 time.
// Returns false (and leaves `out` empty) when NTP time is not yet available.
bool formatIsoTime(uint32_t atUptime, char* out, size_t len);

void scheduleReboot(uint32_t delayMs);

// Set while an OTA upload is running: scans are paused so the radio and heap stay free.
void setBusy(bool busy);
bool busy();

// Back to the out-of-box state (web page and serial console). Removes the device
// from Home Assistant, deletes config.json (WiFi, MQTT, all settings) and history,
// erases the SDK WiFi area, then reboots into the setup hotspot. Firmware and web
// files stay. Returns false if the config file could not be removed.
bool factoryReset();

}  // namespace System
