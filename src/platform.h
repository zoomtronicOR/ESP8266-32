#pragma once
// Board abstraction: everything that differs between the ESP8266 and ESP32
// Arduino cores lives here, so the rest of the firmware is board-neutral.
#include <Arduino.h>

#include <vector>

#if defined(ESP8266)
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
using WebServerT = ESP8266WebServer;
#elif defined(ESP32)
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
using WebServerT = WebServer;
#else
#error "Unsupported board"
#endif

namespace Platform {

// ---- chip / system ----
uint32_t chipId();
uint32_t flashSize();
String coreVersion();
uint32_t heapTotal();       // for the System page RAM usage bar
uint8_t heapFragmentation();  // percent
uint32_t maxFreeBlock();
String resetReason();
bool resetByWatchdog();
bool resetByCrash();
uint32_t crashCause();        // exception number, 0 if unknown
void eraseWifiConfig();       // SDK copy of WiFi credentials (factory reset)

// ---- file system ----
bool fsBegin();
size_t fsTotal();
size_t fsUsed();
std::vector<String> listDir(const char* dir);  // file names (no path)

// ---- WiFi ----
String macSuffix();  // last 2 bytes of the WiFi MAC as 4 hex digits (valid before WiFi starts)
void wifiPrepare(const char* hostname);  // before mode()/begin(): hostname, no power save
void startScan(uint8_t channel);         // async, include hidden; channel 0 = all
bool scanHidden(int i);
bool scanReportsWidth();                              // true when scanWidth() is real data
void scanWidth(int i, uint8_t& width, int8_t& secondary);  // 20/40 MHz, secondary +1 above / -1 below
bool encIsOpen(uint8_t enc);
const char* securityName(uint8_t enc);
bool staWrongPassword();                 // last connect attempt failed on the password
void startTime(const char* tz, const char* server);
void mdnsUpdate();

}  // namespace Platform
