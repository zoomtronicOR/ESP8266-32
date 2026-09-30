#pragma once
#include <Arduino.h>

// MAC prefix (OUI) -> vendor name, from /oui.bin in LittleFS (built by tools/build_oui.py
// from the IEEE registry, networking/home vendors only). The file stays open and is
// searched on disk, so it costs almost no RAM; lookups happen once per new AP.
namespace Oui {

constexpr uint8_t kUnknown = 0xFF;  // prefix not in the table (or no table)
constexpr uint8_t kLocal = 0xFE;    // locally administered MAC: carries no vendor

bool begin();  // (re)opens /oui.bin; call again after replacing the file
void end();    // close the file, e.g. before it is replaced
uint8_t lookup(const uint8_t* mac);
String name(uint8_t id);  // empty for kUnknown / kLocal
uint16_t vendorCount();
uint32_t entryCount();

}  // namespace Oui
