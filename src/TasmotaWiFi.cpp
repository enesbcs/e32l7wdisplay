#include "TasmotaWiFi.hpp"
#include "LogBuffer.hpp"

#include "nvs.h"
#include "esp_heap_caps.h"

#include <cstring>

static constexpr uint16_t CFG_HOLDER_OK = 4617;
static constexpr uint32_t TEXT_POOL_OFFSET = 0x017;
static constexpr size_t   MAX_STR = 96;
static constexpr uint32_t SSID1 = 4, SSID2 = 5, PWD1 = 6, PWD2 = 7;

static const char *NVS_NS = "main";
static const char *NVS_KEY = "Settings";

static bool readStr(const uint8_t *pool, size_t poolLen, uint32_t index,
                    std::string &out) {
    size_t pos = 0;
    for (uint32_t i = 0; i < index; i++) {
        while (pos < poolLen && pool[pos] != '\0') pos++;
        if (pos >= poolLen) return false;
        pos++; // skip separator
    }
    const uint8_t *start = pool + pos;
    size_t len = 0;
    while (pos < poolLen && pool[pos] != '\0') { pos++; len++; }
    if (len > MAX_STR - 1) len = MAX_STR - 1;
    out.assign((const char *)start, len);
    LogBuffer::logf("Tasmota settings[%u] = '%s'", (unsigned)index, out.c_str());
    return len > 0;
}

bool TasmotaWiFi::readCredentials(std::string &ssid1, std::string &pwd1,
                                  std::string &ssid2, std::string &pwd2) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        LogBuffer::logf("Tasmota NVS ns '%s' open failed: %s", NVS_NS, esp_err_to_name(err));
        return false;
    }
    size_t len = 0;
    err = nvs_get_blob(h, NVS_KEY, nullptr, &len);
    if (err != ESP_OK || len < 2) {
        LogBuffer::logf("Tasmota blob '%s' missing: %s", NVS_KEY, esp_err_to_name(err));
        nvs_close(h);
        return false;
    }
    // NVS blob parse only: PSRAM is fine, saves internal RAM.
    uint8_t *blob = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        nvs_close(h);
        return false;
    }
    err = nvs_get_blob(h, NVS_KEY, blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        heap_caps_free(blob);
        return false;
    }

    uint16_t holder;
    memcpy(&holder, blob + 0x000, 2);
    if (holder != CFG_HOLDER_OK) {
        LogBuffer::logf("Tasmota cfg_holder invalid (0x%04X), skipping blob", holder);
        heap_caps_free(blob);
        return false;
    }
    LogBuffer::logf("Tasmota Settings blob valid (%d bytes)", len);

    size_t poolLen = (len > TEXT_POOL_OFFSET) ? len - TEXT_POOL_OFFSET : 0;
    const uint8_t *pool = blob + TEXT_POOL_OFFSET;

    bool ok = true;
    ok &= !readStr(pool, poolLen, SSID1, ssid1);
    if (!ssid1.empty()) readStr(pool, poolLen, PWD1, pwd1);
    if (!readStr(pool, poolLen, SSID2, ssid2)) ok = false;
    if (!ssid2.empty()) readStr(pool, poolLen, PWD2, pwd2);

    heap_caps_free(blob);

    if (ssid1.empty() && ssid2.empty()) {
        LogBuffer::logf("Tasmota blob has no WiFi creds");
        return false;
    }
    LogBuffer::logf("Tasmota WiFi: ssid1='%s' ssid2='%s'", ssid1.c_str(), ssid2.c_str());
    return ok;
}