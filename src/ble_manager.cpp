#include "ble_manager.h"

#include "config.h"

#if defined(FEATURE_BLE_NATIVE)

#include <NimBLEDevice.h>

#include "statistics.h"
#include "storage.h"
#include "system.h"
#include "wifi_scanner.h"

namespace {

// GATT: one service, one read/notify characteristic with a small JSON summary
const char* const kServiceUuid = "6e5a0001-7a1d-4c3b-9f2e-0e5f5a57a001";
const char* const kSummaryUuid = "6e5a0002-7a1d-4c3b-9f2e-0e5f5a57a001";
const uint16_t kBthomeUuid = 0xFCD2;  // BTHome service data UUID

bool s_started = false;
NimBLECharacteristic* s_summary = nullptr;
uint8_t s_packetId = 0;
uint32_t s_lastAdvMs = 0;

bool s_scanning = false;
uint32_t s_nextScanMs = 5000;
uint32_t s_scanId = 0;
uint16_t s_lastScanDevices = 0;
int8_t s_lastScanStrongest = INT8_MIN;
uint32_t s_lastScanUptime = 0;

Ble::ScanPoint s_hist[BLE_HISTORY_POINTS];
uint16_t s_histCount = 0;
uint16_t s_histNext = 0;

Ble::Device s_devices[BLE_MAX_DEVICES];
uint16_t s_count = 0;

Ble::Device* findOrAdd(const uint8_t* addr) {
    for (uint16_t i = 0; i < s_count; i++) {
        if (memcmp(s_devices[i].addr, addr, 6) == 0) return &s_devices[i];
    }
    Ble::Device* slot = nullptr;
    if (s_count < BLE_MAX_DEVICES) {
        slot = &s_devices[s_count++];
    } else {  // full: reuse the one not seen for the longest time
        slot = &s_devices[0];
        for (uint16_t i = 1; i < s_count; i++) {
            if (s_devices[i].lastSeen < slot->lastSeen) slot = &s_devices[i];
        }
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->addr, addr, 6);
    slot->company = 0xFFFF;
    return slot;
}

// BTHome v2: device info byte, then objects in ascending id order.
// 0x00 packet id, then three "count" (0x09, uint8) objects that Home Assistant
// shows as Count / Count 2 / Count 3: WiFi APs, BLE devices, recommended channel.
void updateAdvertising() {
    if (!s_started) return;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();
    if (!config.bleEnabled) return;

    ScanSummary sum = Scanner::summary();
    ChannelStat cs[Stats::kChannels];
    Stats::analyzeChannels(cs);
    uint8_t rec = Scanner::scanId() ? Stats::recommendedChannel(cs) : 0;

    NimBLEAdvertisementData ad;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    if (config.bleBthome) {
        uint8_t p[] = {0x40,                                  // BTHome v2, not encrypted, regular interval
                       0x00, s_packetId++,                    // packet id (HA drops duplicates)
                       0x09, (uint8_t)min<uint16_t>(sum.present, 255),
                       0x09, (uint8_t)min<uint16_t>(s_lastScanDevices, 255),
                       0x09, rec};
        ad.setServiceData(NimBLEUUID(kBthomeUuid), p, sizeof(p));
    }
    NimBLEAdvertisementData sr;
    sr.setName(config.bleName.c_str());
    adv->setAdvertisementData(ad);
    adv->setScanResponseData(sr);
    adv->enableScanResponse(true);
    // ~1 s interval (units of 0.625 ms): enough for BTHome, leaves the shared radio to WiFi
    adv->setMinInterval(1440);
    adv->setMaxInterval(1760);
    adv->start();

    // GATT summary (read or subscribe with any BLE app)
    char json[120];
    snprintf(json, sizeof(json), "{\"aps\":%u,\"open\":%u,\"channels\":%u,\"strongest\":%d,\"recommended\":%u,\"ble\":%u}",
             sum.present, sum.open, sum.channelsUsed, sum.present ? sum.strongest : 0, rec, s_lastScanDevices);
    s_summary->setValue((const uint8_t*)json, strlen(json));
    s_summary->notify();
}

void startScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);  // passive: we only listen, and names come from advertisements
    scan->setInterval(100);
    scan->setWindow(30);         // 30 % duty, leaves airtime for WiFi (one shared radio)
    scan->setDuplicateFilter(true);
    s_scanning = scan->start(BLE_SCAN_DURATION_MS, false, true);
}

void finishScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEScanResults res = scan->getResults();
    s_scanId++;
    uint32_t now = System::uptimeSeconds();
    s_lastScanDevices = 0;
    s_lastScanStrongest = INT8_MIN;
    int32_t rssiSum = 0;
    for (int i = 0; i < res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        if (!d) continue;
        const uint8_t* a = d->getAddress().getVal();  // little endian
        uint8_t addr[6];
        for (int k = 0; k < 6; k++) addr[k] = a[5 - k];
        Ble::Device* dev = findOrAdd(addr);
        dev->randomAddr = d->getAddress().getType() != BLE_ADDR_PUBLIC;
        dev->rssi = (int8_t)constrain(d->getRSSI(), -128, 0);
        if (d->haveName()) strlcpy(dev->name, d->getName().c_str(), sizeof(dev->name));
        if (d->haveManufacturerData()) {
            std::string m = d->getManufacturerData();
            if (m.size() >= 2) dev->company = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
        }
        dev->lastSeen = now;
        dev->lastScanId = s_scanId;
        if (dev->seen < UINT16_MAX) dev->seen++;
        s_lastScanDevices++;
        if (dev->rssi > s_lastScanStrongest) s_lastScanStrongest = dev->rssi;
        rssiSum += dev->rssi;
    }
    scan->clearResults();
    s_lastScanUptime = now;
    Ble::ScanPoint& p = s_hist[s_histNext];
    p.uptime = now;
    p.devices = (uint8_t)min<uint16_t>(s_lastScanDevices, 255);
    p.strongest = s_lastScanStrongest;
    p.average = s_lastScanDevices ? (int8_t)lroundf((float)rssiSum / s_lastScanDevices) : INT8_MIN;
    s_histNext = (s_histNext + 1) % BLE_HISTORY_POINTS;
    if (s_histCount < BLE_HISTORY_POINTS) s_histCount++;
    s_scanning = false;
    LOGF("BLE scan #%lu: %u devices", (unsigned long)s_scanId, s_lastScanDevices);
    updateAdvertising();
}

}  // namespace

namespace Ble {

bool available() { return true; }

void begin() {
    if (!config.bleEnabled) {
        LOGF("BLE: disabled in settings");
        return;
    }
    NimBLEDevice::init(config.bleName.c_str());
    NimBLEServer* server = NimBLEDevice::createServer();
    server->advertiseOnDisconnect(true);
    NimBLEService* svc = server->createService(kServiceUuid);
    s_summary = svc->createCharacteristic(kSummaryUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_summary->setValue("{}");

    server->start();
    s_started = true;
    updateAdvertising();
    LOGF("BLE: '%s' (%s), BTHome %s, scan %s", config.bleName.c_str(), address().c_str(),
         config.bleBthome ? "on" : "off", config.bleScan ? "on" : "off");
}

void loop() {
    if (!s_started || !config.bleEnabled) return;
    uint32_t now = millis();
    if (s_scanning) {
        if (!NimBLEDevice::getScan()->isScanning()) finishScan();
        return;
    }
    if (config.bleScan && (int32_t)(now - s_nextScanMs) >= 0 && !Scanner::scanning()) {
        s_nextScanMs = now + (uint32_t)config.bleScanInterval * 1000;
        startScan();
        return;
    }
    // Refresh the advertised values after WiFi scans too
    if (now - s_lastAdvMs >= BLE_ADV_REFRESH_MS) {
        s_lastAdvMs = now;
        updateAdvertising();
    }
}

void reconfigure() {
    if (!config.bleEnabled) {
        if (s_started) NimBLEDevice::getAdvertising()->stop();
        return;
    }
    if (!s_started) {
        begin();
        return;
    }
    // The GAP name is fixed at init; a new name applies after a reboot.
    s_nextScanMs = millis();
    updateAdvertising();
}

bool enabled() { return s_started && config.bleEnabled; }
bool advertising() { return s_started && NimBLEDevice::getAdvertising()->isAdvertising(); }
bool scanning() { return s_scanning; }
String address() { return s_started ? String(NimBLEDevice::getAddress().toString().c_str()) : String(); }

uint16_t count() { return s_count; }
const Device& at(uint16_t i) { return s_devices[i]; }
uint32_t scanId() { return s_scanId; }
uint16_t lastScanDevices() { return s_lastScanDevices; }
int8_t lastScanStrongest() { return s_lastScanStrongest; }
uint32_t lastScanUptime() { return s_lastScanUptime; }
uint16_t historyCount() { return s_histCount; }
const ScanPoint& historyAt(uint16_t i) {
    return s_hist[(s_histNext + BLE_HISTORY_POINTS - s_histCount + i) % BLE_HISTORY_POINTS];
}

#else  // no BLE radio on this board -----------------------------------------------------

namespace Ble {

namespace {
Device s_none = {};
}

bool available() { return false; }
void begin() {}
void loop() {}
void reconfigure() {}
bool enabled() { return false; }
bool advertising() { return false; }
bool scanning() { return false; }
String address() { return String(); }
uint16_t count() { return 0; }
const Device& at(uint16_t) { return s_none; }
uint32_t scanId() { return 0; }
uint16_t lastScanDevices() { return 0; }
int8_t lastScanStrongest() { return INT8_MIN; }
uint32_t lastScanUptime() { return 0; }
uint16_t historyCount() { return 0; }
const ScanPoint& historyAt(uint16_t) {
    static ScanPoint none = {};
    return none;
}

#endif

// A few common Bluetooth SIG company identifiers (manufacturer data), for the device list.
const char* companyName(uint16_t id) {
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x038F: return "Xiaomi";
        case 0x0157: return "Huami";
        case 0x0087: return "Garmin";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        case 0x0171: return "Amazon";
        case 0x00D2: return "Bose";
        case 0x0057: return "Harman";
        case 0x0310: return "Sony";
        case 0x012D: return "Sony";
        case 0x00C4: return "LG";
        case 0x027D: return "Huawei";
        case 0x0822: return "Tile";
        default:     return nullptr;
    }
}

}  // namespace Ble
