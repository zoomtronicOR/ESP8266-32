#pragma once

// ---- Firmware ----------------------------------------------------------------
#define FW_NAME    "ESP WiFi Monitor"  // product name is board-neutral (ESP8266 now, ESP32 later)
// ---- Board -------------------------------------------------------------------------
#if defined(ESP8266)
#define BOARD_NAME "ESP8266 NodeMCU"
#define CHIP_NAME  "ESP8266EX"
#define BOARD_ID   "esp8266"  // firmware tag: OTA refuses images built for another board
#define MAX_TRACKED_APS_LIMIT 150  // upper bound for the runtime "max_aps" setting
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define BOARD_NAME "ESP32-C3"
#define CHIP_NAME  "ESP32-C3"
#define BOARD_ID   "esp32c3"
#define MAX_TRACKED_APS_LIMIT 300
#define FEATURE_BLE_NATIVE 1       // built-in Bluetooth LE radio
#define FEATURE_CHANNEL_WIDTH 1    // scan reports 20/40 MHz (HT40 secondary channel)
#else
#error "Add a board block to include/config.h"
#endif
#define FW_VERSION "0.6.0"

// ---- Defaults (overridable at runtime via /api/config, stored in LittleFS) ----
#define DEFAULT_DEVICE_NAME     "wifi-monitor-01"
#define DEFAULT_HOSTNAME        "wifi-monitor"
#define DEFAULT_TIMEZONE        "CET-1CEST,M3.5.0,M10.5.0/3"   // POSIX TZ (Europe/Belgrade)
#define DEFAULT_SCAN_INTERVAL_S 30
#define DEFAULT_MAX_APS         100
#define DEFAULT_HISTORY_HOURS   24
#define DEFAULT_UI_ACCENT       "#2563eb"

#define CONFIG_PATH     "/config.json"
#define CONFIG_TMP_PATH "/config.tmp"

// ---- Scanner -------------------------------------------------------------------
// Scans hop across all channels and briefly take the radio off the home channel,
// so the interval is clamped to protect the web UI / (future) MQTT connection.
#define MIN_SCAN_INTERVAL_S    10
#define MAX_SCAN_INTERVAL_S    1800
#define MANUAL_SCAN_MIN_GAP_MS 5000
#define SCAN_TIMEOUT_MS        15000
#define MIN_TRACKED_APS        10
#define STRONG_RSSI_DBM        (-67)   // ">= this" counts as a strong AP
#define NEW_AP_WINDOW_S        600     // AP first seen after the baseline scan within this window = "new"

// ---- History -------------------------------------------------------------------
// RAM: the last N scans (device summary + RSSI of every tracked AP), any time base.
// Flash: one aggregated record per bucket, written only once NTP time is known.
#define HISTORY_LIVE_POINTS    60
#define HISTORY_BUCKET_S       300
#define HISTORY_DIR            "/hist"
#define HISTORY_MIN_FREE_BYTES (160 * 1024)  // prune oldest day files below this
#define HISTORY_MAX_POINTS     360           // API downsamples to at most this many points

// ---- WiFi manager / setup hotspot ------------------------------------------------
#define AP_SSID_PREFIX          "ESP-WIFI-MONITOR-"
#define AP_IP_ADDR              192, 168, 4, 1
#define STA_CONNECT_TIMEOUT_MS  20000   // no STA link this long -> start fallback AP
#define STA_RETRY_INTERVAL_MS   60000   // while in fallback AP, retry STA this often
#define AP_SHUTDOWN_DELAY_MS    30000   // STA connected this long and no AP clients -> stop AP
#define AP_FORCE_SHUTDOWN_MS    180000  // STA connected this long -> stop AP even with clients
#define DNS_PORT                53      // captive-portal DNS (answers every name with the AP IP)
#define NTP_SERVER              "pool.ntp.org"

// ---- Alerts ------------------------------------------------------------------------
#define ALERT_RING_SIZE           24      // alerts kept in RAM (and reloaded from flash at boot)
#define ALERTS_PATH               "/alerts.bin"
#define KNOWN_PATH                "/known.bin"  // bloom filter of every BSSID ever seen
#define BOOT_PATH                 "/boot.bin"   // boot/reset counters, acknowledged alert id
#define KNOWN_BLOOM_BYTES         512           // 4096 bits: ~1 % false "known" at 300 APs
#define KNOWN_SAVE_INTERVAL_MS    30000         // save new BSSIDs at most every 30 s (and before reboots)
#define KNOWN_BASELINE_SCANS      3             // with no filter yet, the first scans only learn (weak APs come and go)
#define MAX_NEW_AP_ALERTS_PER_SCAN 5            // moving the device somewhere new must not flood
#define DEFAULT_ALERT_STRONG_RSSI (-40)
#define DEFAULT_ALERT_DENSITY_APS 6
#define LOW_MEMORY_BYTES          6000
#define LOW_MEMORY_REARM_BYTES    8000
#define WIFI_OUTAGE_ALERT_S       30
#define MQTT_OUTAGE_ALERT_S       60
#define STABLE_UPTIME_S           600     // after this long, a boot no longer counts toward a restart loop
#define RESTART_LOOP_COUNT        3       // abnormal restarts in a row -> alert

// ---- Hardware ------------------------------------------------------------------------
#if defined(ESP8266)
#define RESET_BUTTON_PIN      0       // NodeMCU "FLASH" button (GPIO0, active low)
#define STATUS_LED_PIN        2       // on-board LED (GPIO2, active low)
#else
#define RESET_BUTTON_PIN      9       // ESP32-C3 "BOOT" button (GPIO9, active low)
#define STATUS_LED_PIN        8       // on-board LED on C3 mini boards (GPIO8, active low)
#endif
#define RESET_BUTTON_HOLD_MS  10000   // hold this long at runtime -> factory reset
#define RESET_BUTTON_BLINK_MS 3000    // LED starts blinking after this long, as a warning

// ---- OTA ---------------------------------------------------------------------------
#define WEBFILE_MAX_BYTES     (200 * 1024)

// ---- Bluetooth LE (boards with FEATURE_BLE_NATIVE) ------------------------------------
#define DEFAULT_BLE_NAME          "WiFi-Monitor"
#define DEFAULT_BLE_SCAN_INTERVAL 60      // s between passive BLE scans
#define MIN_BLE_SCAN_INTERVAL     15
#define BLE_SCAN_DURATION_MS      5000
#define BLE_ADV_REFRESH_MS        30000   // BTHome values / GATT summary refresh
#define BLE_MAX_DEVICES           60      // recently seen BLE devices kept for the web page
#define BLE_HISTORY_POINTS        60      // last BLE scans kept for the signal-over-time chart

// ---- Channel analysis (estimated, not RF measurement) ------------------------------
#define CONGESTION_SCANS          10      // averaged over up to this many recent scans

// ---- MQTT / Home Assistant ------------------------------------------------------
#define DEFAULT_MQTT_PORT        1883
#define DEFAULT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_KEEPALIVE_S         30
#define MQTT_SOCKET_TIMEOUT_MS   2000   // connect() blocks at most this long
#define MQTT_RETRY_MIN_MS        5000   // reconnect backoff 5 s .. 5 min
#define MQTT_RETRY_MAX_MS        300000
#define MQTT_STATE_INTERVAL_MS   60000  // uptime/RSSI/status even without scans

// ---- Web -----------------------------------------------------------------------
#define HTTP_PORT 80
// All POST /api/* requests must carry this header. Browsers cannot add a custom
// header cross-origin without a CORS preflight (which we never answer), so this
// blocks simple CSRF until real authentication lands in Phase 5.
#define CSRF_HEADER       "X-Requested-With"
#define CSRF_HEADER_VALUE "wifi-monitor"

// ---- Not available on ESP8266 hardware (kept for the future BLE-capable board) --
// ESP8266 has no Bluetooth radio and only a 2.4 GHz WiFi radio. These options are
// intentionally disabled there; boards with a BLE radio set FEATURE_BLE_NATIVE in the board block.
// FEATURE_BLE_NATIVE: set in the board block above for boards with a BLE radio
// #define FEATURE_WIFI_5GHZ       1   // 5 GHz scanning (ESP32-C5 / dedicated radio)
// #define FEATURE_WIFI_6GHZ       1   // 6 GHz / WiFi 6E scanning
// #define FEATURE_AIRTIME_UTIL    1   // real channel airtime utilization (needs RF sniffer)
// FEATURE_CHANNEL_WIDTH: set in the board block for boards whose scan reports it (ESP32)
// #define FEATURE_WPA3_DETECTION  1   // ESP8266 scan reports only NONE/WEP/TKIP/CCMP/AUTO
// #define FEATURE_MQTT_TLS        1   // BearSSL needs ~20 KB heap; only ~22 KB are free at runtime
