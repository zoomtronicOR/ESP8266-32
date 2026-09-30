#include "wifi_manager.h"

#include <DNSServer.h>
#include <time.h>

#include "config.h"
#include "platform.h"
#include "storage.h"
#include "system.h"

namespace {

DNSServer s_dns;
String s_apSsid;
bool s_apActive = false;
bool s_wasConnected = false;
bool s_staTrying = false;
bool s_mdns = false;
uint32_t s_lastBeginMs = 0;
uint32_t s_disconnectedSince = 0;
uint32_t s_connectedSince = 0;

// Network picked in the web UI, not yet saved to config
bool s_attemptActive = false;
bool s_attemptStarted = false;
uint32_t s_attemptStartAt = 0;
String s_attemptSsid;
String s_attemptPass;
const char* s_attemptState = "";

bool haveCredentials() { return config.wifiSsid.length() > 0; }

// Static address from the settings, or DHCP (all zeros).
void applyIpConfig(bool allowStatic) {
    IPAddress ip, gw, mask, dns1, dns2;
    if (allowStatic && config.ipStatic && ip.fromString(config.ipAddr) && gw.fromString(config.ipGateway) &&
        mask.fromString(config.ipMask)) {
        if (!dns1.fromString(config.ipDns1)) dns1 = gw;
        dns2.fromString(config.ipDns2);
        WiFi.config(ip, gw, mask, dns1, dns2);
        LOGF("WiFi: static IP %s", config.ipAddr.c_str());
    } else {
        WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));  // DHCP
    }
}

void startSta() {
    LOGF("WiFi: connecting to '%s'", config.wifiSsid.c_str());
    applyIpConfig(true);
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
    s_staTrying = true;
    s_lastBeginMs = millis();
}

void startAp() {
    WiFi.mode(WIFI_AP_STA);
    IPAddress ip(AP_IP_ADDR);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(s_apSsid.c_str());
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(DNS_PORT, "*", ip);
    s_apActive = true;
    LOGF("Setup hotspot started: '%s' at %s", s_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void stopAp() {
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_apActive = false;
    LOGF("Setup hotspot stopped");
}

void finishAttempt(const char* state) {
    s_attemptActive = false;
    s_attemptState = state;
    s_attemptPass = "";
    LOGF("WiFi: attempt to join '%s' -> %s", s_attemptSsid.c_str(), state);
}

// Supervises a connectTo() attempt. Returns true while it owns the STA interface.
bool superviseAttempt(uint32_t now, bool connected) {
    if (!s_attemptActive) return false;
    if (!s_attemptStarted) {
        // Started from loop(), not the HTTP handler, so the "ok" response leaves
        // before the radio switches channel.
        if ((int32_t)(now - s_attemptStartAt) < 0) return true;
        if (!s_apActive) startAp();  // keep a way back in if the new network fails
        WiFi.disconnect();
        // A network picked in the setup page always starts with DHCP: a static
        // address from another network would make the device unreachable.
        applyIpConfig(s_attemptSsid == config.wifiSsid);
        WiFi.begin(s_attemptSsid.c_str(), s_attemptPass.c_str());
        s_attemptStarted = true;
        s_staTrying = true;
        s_lastBeginMs = now;
        return true;
    }
    // After begin() the station can only associate with the picked network, so
    // any established link is the attempt succeeding. Never tear it down here.
    if (connected) {
        if (WiFi.SSID() != s_attemptSsid) {
            LOGF("WiFi: note, SDK reports SSID '%s'", WiFi.SSID().c_str());
        }
        if (s_attemptSsid != config.wifiSsid) config.ipStatic = false;  // new network: DHCP
        config.wifiSsid = s_attemptSsid;
        config.wifiPass = s_attemptPass;
        if (!Storage::saveConfig()) LOGF("WiFi: could not save credentials");
        finishAttempt("connected");
        return false;
    }
    wl_status_t st = WiFi.status();
    if (Platform::staWrongPassword()) {
        finishAttempt("wrong_password");
    } else if (now - s_lastBeginMs > STA_CONNECT_TIMEOUT_MS) {
        finishAttempt(st == WL_NO_SSID_AVAIL ? "no_ssid" : "failed");
    } else {
        return true;
    }
    // Failed: stop trying the picked network; the regular logic resumes with
    // the saved config (if any) once the hotspot is idle.
    WiFi.disconnect();
    s_staTrying = false;
    s_lastBeginMs = now;
    return false;
}

}  // namespace

namespace WifiManager {

void begin() {
    s_apSsid = String(AP_SSID_PREFIX) + Platform::macSuffix();

    WiFi.persistent(false);  // credentials live in config.json, not the SDK flash area
    WiFi.setAutoReconnect(true);
    Platform::wifiPrepare(config.hostname.c_str());

    if (haveCredentials()) {
        WiFi.mode(WIFI_STA);
        startSta();
    } else {
        LOGF("WiFi: no credentials configured");
        startAp();
    }
    s_disconnectedSince = millis();

    s_mdns = MDNS.begin(config.hostname.c_str());
    if (s_mdns) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        LOGF("mDNS: http://%s.local", config.hostname.c_str());
    }
}

void loop() {
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;

    if (connected && !s_wasConnected) {
        s_wasConnected = true;
        s_staTrying = false;
        s_connectedSince = now;
        LOGF("WiFi: connected to '%s', IP %s, RSSI %d dBm, channel %d", WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
        Platform::startTime(config.timezone.c_str(), NTP_SERVER);
    } else if (!connected && s_wasConnected) {
        s_wasConnected = false;
        s_disconnectedSince = now;
        LOGF("WiFi: connection lost");
    }

    bool attempting = superviseAttempt(now, connected);

    if (connected) {
        uint32_t up = now - s_connectedSince;
        if (s_apActive && ((up > AP_SHUTDOWN_DELAY_MS && WiFi.softAPgetStationNum() == 0) ||
                           up > AP_FORCE_SHUTDOWN_MS)) {
            stopAp();
        }
    } else if (!attempting && haveCredentials()) {
        if (!s_apActive && now - s_disconnectedSince > STA_CONNECT_TIMEOUT_MS) {
            LOGF("WiFi: no connection, starting setup hotspot");
            startAp();
        }
        if (s_apActive) {
            uint8_t apClients = WiFi.softAPgetStationNum();
            // STA reconnect attempts make the soft-AP hop channels, which kicks a
            // phone that is trying to reconfigure us. Pause them while the AP is in use.
            if (apClients > 0 && s_staTrying && now - s_lastBeginMs > STA_CONNECT_TIMEOUT_MS) {
                WiFi.disconnect();
                s_staTrying = false;
            } else if (apClients == 0 && now - s_lastBeginMs > STA_RETRY_INTERVAL_MS) {
                startSta();
            }
        }
    }

    if (s_apActive) s_dns.processNextRequest();
    if (s_mdns) Platform::mdnsUpdate();
}

void connectTo(const String& ssid, const String& password) {
    s_attemptSsid = ssid;
    s_attemptPass = password;
    s_attemptActive = true;
    s_attemptStarted = false;
    s_attemptStartAt = millis() + 300;
    s_attemptState = "connecting";
    LOGF("WiFi: trying '%s' (picked in web UI)", ssid.c_str());
}

bool staConnected() { return WiFi.status() == WL_CONNECTED; }
bool apActive() { return s_apActive; }
const String& apSsid() { return s_apSsid; }
const char* attemptState() { return s_attemptState; }
const String& attemptSsid() { return s_attemptSsid; }

const char* modeName() {
    switch (WiFi.getMode()) {
        case WIFI_STA:    return "STA";
        case WIFI_AP:     return "AP";
        case WIFI_AP_STA: return "AP+STA";
        default:          return "OFF";
    }
}

bool scanAllowed() {
    if (s_attemptActive) return false;
    if (staConnected()) return true;
    if (!s_apActive) return false;
    return !(s_staTrying && millis() - s_lastBeginMs < STA_CONNECT_TIMEOUT_MS);
}

}  // namespace WifiManager
