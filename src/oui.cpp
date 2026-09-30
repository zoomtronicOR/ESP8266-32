#include "oui.h"

#include <LittleFS.h>

#include "system.h"

namespace {

const char* const kPath = "/oui.bin";
File s_file;
uint16_t s_vendors = 0;
uint16_t s_nameSize = 0;
uint32_t s_entries = 0;
uint32_t s_entriesOffset = 0;

}  // namespace

namespace Oui {

void end() {
    if (s_file) s_file.close();
    s_entries = 0;
    s_vendors = 0;
}

bool begin() {
    end();
    s_file = LittleFS.open(kPath, "r");
    if (!s_file) {
        LOGF("OUI: %s missing, vendor names off", kPath);
        return false;
    }
    uint8_t h[12];
    if (s_file.read(h, sizeof(h)) != (int)sizeof(h) || memcmp(h, "OUI1", 4) != 0) {
        LOGF("OUI: %s has an unknown format", kPath);
        s_file.close();
        return false;
    }
    s_vendors = h[4] | (h[5] << 8);
    s_nameSize = h[6] | (h[7] << 8);
    s_entries = h[8] | (h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
    s_entriesOffset = sizeof(h) + (uint32_t)s_vendors * s_nameSize;
    LOGF("OUI: %lu prefixes, %u vendors", (unsigned long)s_entries, s_vendors);
    return true;
}

uint8_t lookup(const uint8_t* mac) {
    if (mac[0] & 0x02) return kLocal;  // U/L bit: locally administered (random, guest, mesh BSSIDs)
    if (!s_file || !s_entries) return kUnknown;
    uint32_t key = ((uint32_t)mac[0] << 16) | (mac[1] << 8) | mac[2];
    uint32_t lo = 0, hi = s_entries;
    while (lo < hi) {  // binary search over sorted 4-byte records on disk
        uint32_t mid = (lo + hi) / 2;
        uint8_t e[4];
        if (!s_file.seek(s_entriesOffset + mid * 4) || s_file.read(e, 4) != 4) return kUnknown;
        uint32_t oui = ((uint32_t)e[0] << 16) | (e[1] << 8) | e[2];
        if (oui == key) return e[3];
        if (oui < key) lo = mid + 1;
        else hi = mid;
    }
    return kUnknown;
}

String name(uint8_t id) {
    if (!s_file || id >= s_vendors || s_nameSize == 0 || s_nameSize > 32) return String();
    char buf[33];
    if (!s_file.seek(12 + (uint32_t)id * s_nameSize) || s_file.read((uint8_t*)buf, s_nameSize) != s_nameSize)
        return String();
    buf[s_nameSize] = '\0';
    return String(buf);
}

uint16_t vendorCount() { return s_vendors; }
uint32_t entryCount() { return s_entries; }

}  // namespace Oui
