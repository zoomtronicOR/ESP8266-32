#include "wifi_utils.h"

#include <ESP8266WiFi.h>

uint16_t channelToFrequency(uint8_t channel) {
    if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
    if (channel == 14) return 2484;
    return 0;
}

const char* securityName(uint8_t encType) {
    switch (encType) {
        case ENC_TYPE_NONE: return "Open";
        case ENC_TYPE_WEP:  return "WEP";
        case ENC_TYPE_TKIP: return "WPA";
        case ENC_TYPE_CCMP: return "WPA2";
        case ENC_TYPE_AUTO: return "WPA/WPA2";
        default:            return "Unknown";
    }
}

void formatBssid(const uint8_t* b, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

void appendJsonString(String& out, const char* s) {
    out += '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') {
            out += F("\\\"");
        } else if (c == '\\') {
            out += F("\\\\");
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            out += esc;
        } else {
            out += (char)c;
        }
    }
    out += '"';
}
