#include "console.h"

#include <WiFiUdp.h>
#include <lwip/apps/sntp.h>

#include "platform.h"

#include "history.h"
#include "storage.h"
#include "system.h"
#include "wifi_manager.h"
#include "wifi_scanner.h"

namespace {

String s_line;

// Splits into whitespace-separated tokens; "double quotes" keep spaces.
int tokenize(const String& line, String* out, int max) {
    int n = 0;
    size_t i = 0;
    while (i < line.length() && n < max) {
        while (i < line.length() && line[i] == ' ') i++;
        if (i >= line.length()) break;
        String tok;
        if (line[i] == '"') {
            for (i++; i < line.length() && line[i] != '"'; i++) tok += line[i];
            i++;  // closing quote
        } else {
            for (; i < line.length() && line[i] != ' '; i++) tok += line[i];
        }
        out[n++] = tok;
    }
    return n;
}

void printStatus() {
    bool up = WifiManager::staConnected();
    LOGF("WiFi: mode %s, %s", WifiManager::modeName(),
         up ? (String("connected to '") + WiFi.SSID() + "' IP " + WiFi.localIP().toString() + " RSSI " +
               WiFi.RSSI() + " dBm").c_str()
            : "not connected");
    if (WifiManager::apActive()) {
        LOGF("Hotspot: '%s' at %s, %u clients", WifiManager::apSsid().c_str(),
             WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum());
    }
    if (*WifiManager::attemptState()) {
        LOGF("Last join attempt: '%s' -> %s", WifiManager::attemptSsid().c_str(), WifiManager::attemptState());
    }
    if (up) {
        LOGF("DNS %s / %s, NTP server '%s' reachability 0x%02x, time %lu", WiFi.dnsIP(0).toString().c_str(),
             WiFi.dnsIP(1).toString().c_str(), sntp_getservername(0) ? sntp_getservername(0) : "(none)",
             (unsigned)sntp_getreachability(0), (unsigned long)time(nullptr));
    }
    LOGF("Saved network: %s", config.wifiSsid.length() ? config.wifiSsid.c_str() : "(none)");
    LOGF("Scans: %lu, tracked APs %u, free heap %u, time %s", (unsigned long)Scanner::scanId(), Scanner::count(),
         (unsigned)ESP.getFreeHeap(), System::timeSynced() ? "synced" : "not synced");
}

void execute(const String& line) {
    String t[3];
    int n = tokenize(line, t, 3);
    if (n == 0) return;
    if (t[0] == "status") {
        printStatus();
    } else if (t[0] == "wifi" && n >= 2) {
        if (t[1].length() > 32 || (t[2].length() && (t[2].length() < 8 || t[2].length() > 63))) {
            LOGF("wifi: SSID max 32 bytes, password empty or 8-63 characters");
            return;
        }
        WifiManager::connectTo(t[1], n >= 3 ? t[2] : String());
    } else if (t[0] == "ntp" && n >= 2) {
        // lwIP keeps the pointer, so the name must outlive this call
        static char server[64];
        strlcpy(server, t[1].c_str(), sizeof(server));
        Platform::startTime(config.timezone.c_str(), server);
        LOGF("NTP: restarted with server '%s'", server);
    } else if (t[0] == "ntptest" && n >= 2) {
        // Diagnostic only: one raw NTP request, waits up to 2 s for the answer.
        IPAddress ip;
        if (!WiFi.hostByName(t[1].c_str(), ip)) {
            LOGF("ntptest: cannot resolve '%s'", t[1].c_str());
            return;
        }
        WiFiUDP udp;
        udp.begin(0);
        uint8_t pkt[48] = {0x1b};
        udp.beginPacket(ip, 123);
        udp.write(pkt, sizeof(pkt));
        bool sent = udp.endPacket();
        uint32_t start = millis();
        int len = 0;
        while (millis() - start < 2000 && (len = udp.parsePacket()) == 0) delay(10);
        if (len >= 48) {
            udp.read(pkt, 48);
            uint32_t secs = ((uint32_t)pkt[40] << 24 | pkt[41] << 16 | pkt[42] << 8 | pkt[43]) - 2208988800UL;
            LOGF("ntptest: %s answered in %lu ms, epoch %lu", ip.toString().c_str(), millis() - start,
                 (unsigned long)secs);
        } else {
            LOGF("ntptest: %s no answer (sent=%d)", ip.toString().c_str(), sent);
        }
        udp.stop();
    } else if (t[0] == "reboot") {
        System::scheduleReboot(100);
    } else if (t[0] == "factory") {
        System::factoryReset();
    } else {
        LOGF("Commands: status | wifi \"<ssid>\" \"<password>\" | ntp <server> | reboot | factory");
    }
}

}  // namespace

namespace Console {

void loop() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            s_line.trim();
            execute(s_line);
            s_line = "";
        } else if (s_line.length() < 160) {
            s_line += c;
        }
    }
}

}  // namespace Console
