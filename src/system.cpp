#include "system.h"

#include <time.h>

#include "alerts.h"
#include "config.h"
#include "history.h"
#include "mqtt_manager.h"
#include "storage.h"

namespace {
uint32_t s_rebootAt = 0;
bool s_rebootPending = false;
bool s_busy = false;
uint32_t s_buttonDownMs = 0;

// Embedded in the image so the OTA page can read version/board from a .bin before
// uploading it. Format: "ESPWM-FW|<version>|<board>|"
const char kFirmwareTag[] PROGMEM = "ESPWM-FW|" FW_VERSION "|" BOARD_ID "|";

// FLASH button held for RESET_BUTTON_HOLD_MS -> factory reset (for a new owner, or when
// the device is unreachable). The LED blinks during the last seconds as a warning.
void checkResetButton() {
    bool down = digitalRead(RESET_BUTTON_PIN) == LOW;
    uint32_t now = millis();
    if (!down) {
        if (s_buttonDownMs) digitalWrite(STATUS_LED_PIN, HIGH);  // LED off
        s_buttonDownMs = 0;
        return;
    }
    if (!s_buttonDownMs) s_buttonDownMs = now ? now : 1;
    uint32_t held = now - s_buttonDownMs;
    if (held >= RESET_BUTTON_BLINK_MS) digitalWrite(STATUS_LED_PIN, (now / 125) % 2 ? LOW : HIGH);
    if (held >= RESET_BUTTON_HOLD_MS) {
        LOGF("FLASH button held %lu s: factory reset", (unsigned long)(held / 1000));
        s_buttonDownMs = 0;
        digitalWrite(STATUS_LED_PIN, LOW);
        System::factoryReset();
    }
}
}  // namespace

namespace System {

void begin() {
    LOGF("Reset reason: %s", ESP.getResetReason().c_str());
    LOGF("%s", String(FPSTR(kFirmwareTag)).c_str());
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);
}

void loop() {
    uptimeSeconds();
    if (!s_rebootPending) checkResetButton();
    if (s_rebootPending && (int32_t)(millis() - s_rebootAt) >= 0) {
        Alerts::flush();
        LOGF("Rebooting...");
        Serial.flush();
        ESP.restart();
    }
}

uint32_t uptimeSeconds() {
    static uint32_t lastMs = 0;
    static uint32_t rollovers = 0;
    uint32_t now = millis();
    if (now < lastMs) rollovers++;
    lastMs = now;
    uint64_t ms = ((uint64_t)rollovers << 32) | now;
    return (uint32_t)(ms / 1000);
}

bool timeSynced() {
    return time(nullptr) > 1600000000;  // any date after 2020 means NTP has answered
}

bool formatIsoTime(uint32_t atUptime, char* out, size_t len) {
    if (len) out[0] = '\0';
    if (!timeSynced()) return false;
    time_t t = time(nullptr) - (time_t)(uptimeSeconds() - atUptime);
    struct tm tmLocal;
    localtime_r(&t, &tmLocal);
    return strftime(out, len, "%Y-%m-%dT%H:%M:%S", &tmLocal) > 0;
}

bool factoryReset() {
    Mqtt::forgetDevice();
    History::clear();
    Alerts::clear();
    bool ok = Storage::factoryReset();
    ESP.eraseConfig();  // SDK copy of WiFi credentials, in case anything ever stored one
    scheduleReboot(500);
    return ok;
}

void setBusy(bool busy) { s_busy = busy; }
bool busy() { return s_busy; }

void scheduleReboot(uint32_t delayMs) {
    s_rebootAt = millis() + delayMs;
    s_rebootPending = true;
}

}  // namespace System
