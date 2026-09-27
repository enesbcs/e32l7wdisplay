#include "BLEScan.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"

#include "esp_bt.h"
#include "esp_coexist.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include <cstring>
#include <cstdio>

// DIAGNOSTIC (off): per-advert hex dump for BLE decoder work. Enable with
// the build flag -DBLE_ADV_DUMP=1 (e.g. pio run -e esp32-s3 -DBLE_ADV_DUMP=1
// via extra build_flags, or uncomment below). Off in normal builds.
// #define BLE_ADV_DUMP 1
#ifndef BLE_ADV_DUMP
#define BLE_ADV_DUMP 0
#endif

// Tracks whether begin() fully ran so end() is a no-op when the stack was
// never started (e.g. DISABLE_BLE_TEST build, or non-BLE data source).
static bool s_bleStarted = false;
// Init-once pattern: the NimBLE port is initialized a single time and never
// deinitialized at runtime. Full teardown (port_stop/freetos_deinit/
// port_deinit/controller deinit) races with the host task and asserts in
// npl_freertos_event_deinit. begin()/end() only start/stop discovery.
static bool s_portInited = false;
static bool s_synced = false;

static void macStr(const char *mac, char *out13) {
    // BLE addresses are LSB-first on air; other scanners show MSB-first.
    for (int i = 0; i < 6; i++) snprintf(&out13[i * 2], 3, "%02x", (uint8_t)mac[5 - i]);
}

static void bleUpsert(const char *mac, bool hasT, float t, bool hasH, float h,
                      bool hasB, float b, const char *mfg) {
    char id[13];
    macStr(mac, id);
    SensorRegistry::upsert(id, hasT, t, hasH, h, hasB, b);
    if (mfg && *mfg) SensorRegistry::setManufacturer(id, mfg);
    // short serial echo, only for decoded (known) devices
    char msg[64];
    int o = snprintf(msg, sizeof(msg), "ble:%s", id);
    if (hasT && o < (int)sizeof(msg)) o += snprintf(msg + o, sizeof(msg) - o, " T:%.1f", (double)t);
    if (hasH && o < (int)sizeof(msg)) o += snprintf(msg + o, sizeof(msg) - o, " H:%.1f", (double)h);
    if (hasB && o < (int)sizeof(msg)) o += snprintf(msg + o, sizeof(msg) - o, " B:%.0f", (double)b);
    LogBuffer::logf("%s", msg);
}

// Known envelope? BTHome/Xiaomi/ATC only - everything else is noise.
static bool advKnown(const uint8_t *adv, int len) {
    int i = 0;
    while (i + 1 < len) {
        uint8_t advLen = adv[i];
        if (advLen == 0 || i + 1 + advLen > len) break;
        uint8_t advType = adv[i + 1];
        const uint8_t *payload = adv + i + 2;
        int pLen = advLen - 1;
        if (advType == 0xFF && pLen >= 2) {
            uint16_t c = payload[0] | (payload[1] << 8);
            if (c == 0x00D2 || c == 0x0007) return true;
        } else if (advType == 0x16 && pLen >= 2) {
            uint16_t u = payload[0] | (payload[1] << 8);
            if (u == 0xFCD2 || u == 0x181A || u == 0xFE95) return true;
        }
        i += advLen + 1;
    }
    return false;
}

// --- BTHome v2 plaintext ----------------------------------------------------
static void parseBTHome(const uint8_t *data, int len, const char *mac, const char *mfg) {
    if (len < 1) return;
    if (data[0] & 0x02) return; // encrypted - skip
    int i = 1;
    if (i < len && (data[i] == 0x02 || data[i] == 0x03 || data[i] == 0x04)) i++; // optional device info id
    bool hasT = false, hasH = false, hasB = false;
    float t = 0, h = 0, b = 0;
    while (i + 1 < len) {
        uint8_t id = data[i++];
        uint8_t l = data[i++];
        if (i + l > len) break;
        const uint8_t *v = data + i;
        switch (id) {
            case 0x01: // battery %
                if (l == 1) { hasB = true; b = v[0]; }
                break;
            case 0x02: // temperature
                if (l == 2) { hasT = true; t = (int16_t)((v[0] | (v[1] << 8))) / 100.0f; }
                else if (l == 4) { uint32_t u = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24); hasT = true; t = *(float *)&u / 10000.0f; }
                break;
            case 0x03: // humidity
                if (l == 2) { hasH = true; h = ((float)(v[0] | (v[1] << 8))) / 100.0f; }
                break;
            default:
                break;
        }
        i += l;
    }
    if (hasT || hasH || hasB) bleUpsert(mac, hasT, t, hasH, h, hasB, b, mfg);
}

// --- Xiaomi MiBeacon / ATC1441 ----------------------------------------------
static void parseObjectTriples(const uint8_t *p, int len, const char *mac, const char *mfg) {
    bool hasT = false, hasH = false, hasB = false;
    float t = 0, h = 0, b = 0;
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = p[i];
        if (id >= 0x10 || id == 0) break; // 0x10 = end marker
        uint8_t l = p[i + 1];
        if (i + 2 + l > len) break;
        const uint8_t *v = p + i + 2;
        switch (id) {
            case 0x04: // temperature °C, int16/100
                if (l >= 2) {
                    hasT = true;
                    int16_t raw = (int16_t)(v[0] | (v[1] << 8));
                    t = raw / 100.0f;
                }
                break;
            case 0x06: // humidity, uint16/100
                if (l >= 2) { hasH = true; h = (float)(v[0] | (v[1] << 8)) / 100.0f; }
                break;
            case 0x0A: // battery %
                if (l >= 1) { hasB = true; b = v[0]; }
                break;
            default:
                break;
        }
        i += 2 + l;
    }
    if (hasT || hasH || hasB) bleUpsert(mac, hasT, t, hasH, h, hasB, b, mfg);
}

static void parseMiBeacon(const uint8_t *data, int len, const char *mac, const char *mfg) {
    if (len < 3) return;
    uint8_t type = data[0];
    int start = -1;
    if (type == 0x04 || type == 0x05) {
        // standard MiBeacon: type + 6 byte MAC + data objects
        start = 7;
        if (len <= start) return;
        const uint8_t *p = data + start;
        parseObjectTriples(p, len - start, mac, mfg);
    } else if (type == 0x0A) {
        // ATC1441 / ATC custom: type + magic(0xA5) + id + objects
        //   some builds: [0x0A][0xA5][id][0x10]... ; try triples at offset 3
        if (len >= 4 && data[1] == 0xA5) {
            const uint8_t *p = data + 3;
            parseObjectTriples(p, len - 3, mac, mfg);
        }
    }
}

// Real BTHome v2 (service data UUID 0xFCD2): [devinfo][id+fixed-size objs...]
// NOTE: v2 objects carry NO length byte (unlike the legacy TLV parser above).
static void parseBTHomeV2(const uint8_t *data, int len, const char *mac, const char *mfg) {
    if (len < 2) return;
    if (data[0] & 0x01) return; // encrypted - skip (no bindkey support)
    bool hasT = false, hasH = false, hasB = false;
    float t = 0, h = 0, b = 0;
    int i = 1;
    while (i < len) {
        uint8_t id = data[i++];
        switch (id) {
            case 0x00: // packet id, u8
                if (i + 1 > len) return;
                i += 1;
                break;
            case 0x01: // battery %, u8
                if (i + 1 > len) return;
                hasB = true; b = data[i]; i += 1;
                break;
            case 0x02: // temperature, s16/100
                if (i + 2 > len) return;
                hasT = true;
                t = (int16_t)(data[i] | (data[i + 1] << 8)) / 100.0f;
                i += 2;
                break;
            case 0x03: // humidity, u16/100
                if (i + 2 > len) return;
                hasH = true;
                h = (float)(data[i] | (data[i + 1] << 8)) / 100.0f;
                i += 2;
                break;
            case 0x04: // pressure, u24 Pa/100
            case 0x05: // illuminance, u24 lux/100
            case 0x0A: // energy, u24 kWh/1000
            case 0x0B: // power, u24 W/100
                if (i + 3 > len) return;
                i += 3;
                break;
            case 0x06: // mass kg, u16/100
            case 0x07: // mass lb, u16/100
            case 0x08: // dewpoint, s16/100
            case 0x0C: // voltage, u16 mV
            case 0x0D: // pm2.5, u16 ug/m3
            case 0x0E: // pm10, u16 ug/m3
            case 0x12: // co2, u16 ppm
            case 0x13: // tvoc, u16 ug/m3
                if (i + 2 > len) return;
                i += 2;
                break;
            case 0x0F: // generic boolean, u8
            case 0x10: // light (0/1), u8
            case 0x11: // opening (0 shut/1 open), u8
            case 0x15: // battery_ok, u8
            case 0x16: // battery_charging, u8
            case 0x17: // co, u8
            case 0x18: // cold, u8
            case 0x1A: // door, u8
            case 0x1B: // garage_door, u8
            case 0x1C: // gas, u8
            case 0x1D: // heat, u8
            case 0x1E: // light, u8
            case 0x1F: // lock, u8
            case 0x20: // moisture_warn, u8
            case 0x21: // motion, u8
            case 0x2D: // window, u8
            case 0x2E: // humidity, u8
            case 0x2F: // moisture, u8
            case 0x3A: // button, u8
                if (i + 1 > len) return;
                if (id == 0x2E && !hasH) { hasH = true; h = data[i]; }
                i += 1;
                break;
            case 0x14: // moisture, u16*0.01
            case 0x45: // temperature, s16*0.1
            case 0x4A: // voltage, u16*0.1
                if (i + 2 > len) return;
                if (id == 0x45 && !hasT) {
                    hasT = true;
                    t = (int16_t)(data[i] | (data[i + 1] << 8)) * 0.1f;
                }
                i += 2;
                break;
            case 0x3F: // rotation, s16*0.1
                if (i + 2 > len) return;
                i += 2;
                break;
            default:
                return; // unknown/variable-size id, no length info -> keep decoded
        }
    }
    if (hasT || hasH || hasB) bleUpsert(mac, hasT, t, hasH, h, hasB, b, mfg);
}

// ATC / pvvx custom firmware (service data UUID 0x181A).
// Layouts per ble-pasv-mqtt-gw.js reference:
//   atc1441 (13B payload): [MAC(6)][temp BE s16/10][hum u8][batt u8][batt_mv BE u16][frame u8]
//   atc_custom (14-15B):   [MAC(6)][temp LE s16*0.01][hum LE u16*0.01][batt_mv LE u16][batt u8][frame u8]
static void parseAtc(const uint8_t *p, int n, const char *mac, const char *mfg) {
    if (n == 13) {
        float t = (int16_t)((p[6] << 8) | p[7]) / 10.0f;
        bleUpsert(mac, true, t, true, (float)p[8], true, (float)p[9], mfg);
    } else if (n == 14 || n == 15) {
        float t = (int16_t)(p[6] | (p[7] << 8)) * 0.01f;
        float h = (float)(p[8] | (p[9] << 8)) * 0.01f;
        bleUpsert(mac, true, t, true, h, true, (float)p[12], mfg);
    }
}

// Xiaomi MiBeacon (service data UUID 0xFE95).
// Header [FC(2)][DID(2)][FCnt(1)][MAC(6)] = 11B, optional Cap(1B),
// then TLV objects [eid_lo][eid_hi][len][val...]. Reference: ble-pasv-mqtt-gw.js
// eid dispatch on the LOW byte (hi is 0x00 or 0x10 depending on frame),
// mirroring the proven ble-pasv-mqtt-gw.js reference behavior.
static bool xiaomiEidKnown(uint8_t lo, uint8_t hi) {
    if (hi != 0x00 && hi != 0x10) return false;
    return lo == 0x04 || lo == 0x06 || lo == 0x0A || lo == 0x0D;
}

static void parseXiaomiObjs(const uint8_t *p, int n, const char *mac, const char *mfg) {
    bool hasT = false, hasH = false, hasB = false;
    float t = 0, h = 0, b = 0;
    int i = 0;
    while (i + 3 <= n) {
        uint8_t lo = p[i], hi = p[i + 1];
        uint8_t l = p[i + 2];
        if (i + 3 + l > n) break;
        const uint8_t *v = p + i + 3;
        if (hi == 0x00 || hi == 0x10) {
            switch (lo) {
                case 0x04: // temperature, s16/10
                    if (l >= 2) { hasT = true; t = (int16_t)(v[0] | (v[1] << 8)) / 10.0f; }
                    break;
                case 0x06: // humidity, u16/10
                    if (l >= 2) { hasH = true; h = (float)(v[0] | (v[1] << 8)) / 10.0f; }
                    break;
                case 0x0A: // battery %, u8
                    if (l >= 1) { hasB = true; b = v[0]; }
                    break;
                case 0x0D: // temperature+humidity, s16+u16 /10
                    if (l >= 4) {
                        hasT = true; t = (int16_t)(v[0] | (v[1] << 8)) / 10.0f;
                        hasH = true; h = (float)(v[2] | (v[3] << 8)) / 10.0f;
                    }
                    break;
                default:
                    break;
            }
        }
        i += 3 + l;
    }
    if (hasT || hasH || hasB) bleUpsert(mac, hasT, t, hasH, h, hasB, b, mfg);
}

static void parseXiaomi(const uint8_t *p, int n, const char *mac, const char *mfg) {
    // try object start with and without the capability byte (device-dependent)
    for (int k = 0; k < 2; k++) {
        int base = 11 + k;
        if (base + 3 > n) continue;
        if (xiaomiEidKnown(p[base], p[base + 1])) {
            parseXiaomiObjs(p + base, n - base, mac, mfg);
            return;
        }
    }
}

// Walk the AD structures of one advertisement and feed the decoders.
// Stack-agnostic: same payload layout as the Bluedroid version produced.
static void nimbleAdWalk(const uint8_t *adv, int len, const char *mac) {
    int i = 0;
    while (i + 1 < len) {
        uint8_t advLen = adv[i];
        if (advLen == 0 || i + 1 + advLen > len) break;
        uint8_t advType = adv[i + 1];
        const uint8_t *payload = adv + i + 2;
        int pLen = advLen - 1;
        if (advType == 0xFF && pLen >= 2) { // manufacturer specific
            uint16_t company = payload[0] | (payload[1] << 8);
            const uint8_t *p = payload + 2;
            int n = pLen - 2;
            if (company == 0x00D2) { // legacy TLV path
                parseBTHome(p, n, mac, "BTHome");
            } else if (company == 0x0007) { // Xiaomi
                parseMiBeacon(p, n, mac, "Xiaomi");
            }
        } else if (advType == 0x16 && pLen >= 2) { // service data, 16-bit UUID
            uint16_t uuid = payload[0] | (payload[1] << 8);
            const uint8_t *p = payload + 2;
            int n = pLen - 2;
            if (uuid == 0xFCD2) {
                parseBTHomeV2(p, n, mac, "BTHome");
            } else if (uuid == 0x181A) {
                parseAtc(p, n, mac, "ATC");
            } else if (uuid == 0xFE95) {
                parseXiaomi(p, n, mac, "Xiaomi");
            }
            // else: unknown service UUID - visible in BLE_ADV_DUMP,
            // never fed to the registry blindly.
        }
        i += advLen + 1;
    }
}

static int nimbleGapEvent(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (!advKnown(event->disc.data, event->disc.length_data)) return 0;
        const char *mac = (const char *)event->disc.addr.val;
        // RSSI refreshes on every advert, even without new T/H/B data
        if (event->disc.rssi != 127) {
            char rid[13];
            macStr(mac, rid);
            SensorRegistry::setRssi(rid, (int)event->disc.rssi);
        }
#if BLE_ADV_DUMP
        {
            char macstr[13];
            macStr(mac, macstr);
            int n = event->disc.length_data;
            if (n > 32) n = 32;
            char hex[65];
            for (int i = 0; i < n; i++) snprintf(&hex[i * 2], 3, "%02x", event->disc.data[i]);
            hex[n * 2] = '\0';
            LogBuffer::logfSerial("ble_adv:%s type:%u rssi:%d len:%d %s",
                macstr, (unsigned)event->disc.event_type, (int)event->disc.rssi,
                event->disc.length_data, hex);
        }
#endif
        nimbleAdWalk(event->disc.data, event->disc.length_data, mac);
    }
    return 0;
}

static void nimbleStartDiscovery() {
    struct ble_gap_disc_params params = {};
    params.itvl = 0x100;   // 160ms
    params.window = 0x30;  // 30ms (~19% duty, still catches 1-2s adverts)
    params.passive = 1;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    params.limited = 0;
    params.filter_duplicates = 0; // report all (RSSI refresh), like before
    params.disable_observer_mode = 0;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params,
                          nimbleGapEvent, nullptr);
    if (rc != 0) {
        LogBuffer::logfSerial("NimBLE discovery start failed: %d", rc);
    } else {
        LogBuffer::logfSerial("BLE passive scan started (NimBLE, coex: prefer wifi)");
    }
}

static void nimbleOnSync() {
    s_synced = true;
    if (s_bleStarted) nimbleStartDiscovery();
}

static void nimbleOnReset(int reason) {
    s_synced = false;
    LogBuffer::logf("NimBLE host reset: %d", reason);
}

static void nimbleHostTask(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BLEScan::begin() {
    if (!s_portInited) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT); // one-shot, ignore rc
        // NOTE: nimble_port_init() performs esp_bt_controller_init() +
        // esp_bt_controller_enable(BLE) itself (CONFIG_BT_CONTROLLER_ENABLED).
        // Doing it here first makes the port's init fail with
        // "controller init failed" (double init).
        esp_err_t ret = nimble_port_init();
        if (ret != ESP_OK) {
            LogBuffer::logfSerial("NimBLE port init failed: %s", esp_err_to_name(ret));
            return;
        }
        // WiFi keeps RF priority (mgmt-frame alloc fails -> "wifi:m f null").
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        ble_hs_cfg.sync_cb = nimbleOnSync;
        ble_hs_cfg.reset_cb = nimbleOnReset;
        nimble_port_freertos_init(nimbleHostTask);
        s_portInited = true;
        LogBuffer::logfSerial("NimBLE host started (observer, PSRAM alloc)");
    }
    s_bleStarted = true;
    // sync_cb fires async on first init; on re-begin the host is already
    // synced so start discovery directly.
    if (s_synced) nimbleStartDiscovery();
}

void BLEScan::end() {
    if (!s_bleStarted) return;
    s_bleStarted = false;
    ble_gap_disc_cancel(); // safe when idle (rc ignored)
    LogBuffer::logf("BLE scan stopped");
}