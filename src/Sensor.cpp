#include "Sensor.hpp"
#include "LogBuffer.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdio>
#include <new>
#include <cmath>
#include <limits>
#include <utility>

struct SensorEntry {
    SensorReading reading;
    PsramString nameOverride;
    PsramString autoName;   // HA device name / prettified entity
    PsramString manufacturer;
    PsramString device;     // raw HA device_id ("" when none)
    PsramVector<PsramString> members; // member entity_ids (HA)
};

// std::less<> (transparent): find(const char*) needs no temporary key
// allocation - every lookup used to malloc+free a PSRAM key copy, churning
// the shared OPI bus under the RGB scanout's nose on every HA event.
// operator[] still takes key_type (same-type, no temp either).
static std::map<PsramString, SensorEntry, std::less<>,
                PsramAllocator<std::pair<const PsramString, SensorEntry>>> *s_sensors = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;

// Bounded take: a wedged holder must never wedge callers (the display
// prefers one stale frame over a stuck task; dropped sensor updates
// re-arrive within seconds; dropped log lines are acceptable loss).
static bool takeSensor() {
    if (!s_mutex) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(150)) == pdTRUE) return true;
    static uint32_t drops = 0;
    if (++drops % 50 == 1)
        LogBuffer::logfSerial("SensorRegistry: lock busy, update dropped");
    return false;
}

bool SensorRegistry::init() {
    if (s_sensors) return true;
    s_mutex = xSemaphoreCreateMutex();
    using MapType = std::map<PsramString, SensorEntry, std::less<>,
                             PsramAllocator<std::pair<const PsramString, SensorEntry>>>;
    void *mem = heap_caps_malloc(sizeof(MapType), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sensors = mem ? new (mem) MapType() : new MapType();
    LogBuffer::logf("SensorRegistry initialized");
    return true;
}

void SensorRegistry::upsert(const PsramString &id, bool hasT, float t, bool hasH, float h,
                            bool hasB, float b) {
    if (!s_sensors) return;
    // central sanitizer (all sources, display + mirror + WebUI at once):
    // NaN is never stored (a NaN cache never matches -> per-second redraw
    // churn from one stuck sensor); battery is clamped to the drawable range
    if (hasT && std::isnan(t)) hasT = false;
    if (hasH && std::isnan(h)) hasH = false;
    if (hasB) {
        if (std::isnan(b)) hasB = false;
        else if (b < 0) b = 0;
        else if (b > 100) b = 100;
    }
    if (!takeSensor()) return;
    auto &entry = (*s_sensors)[id];
    if (hasT) {
        entry.reading.hasTemperature = true;
        entry.reading.temperature = t;
    }
    if (hasH) {
        entry.reading.hasHumidity = true;
        entry.reading.humidity = h;
    }
    if (hasB) {
        entry.reading.hasBattery = true;
        entry.reading.battery = b;
    }
    if (hasT || hasH || hasB) {
        entry.reading.lastUpdateMillis = (uint32_t)(esp_timer_get_time() / 1000);
    }
    xSemaphoreGive(s_mutex);
}

bool SensorRegistry::get(const PsramString &id, SensorReading &out) {
    if (!s_sensors) return false;
    bool found = false;
    if (!takeSensor()) return false;
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) {
        out = it->second.reading;
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void SensorRegistry::getAllCells(std::vector<std::string> &ids) {
    ids.clear();
    if (!s_sensors) return;
    if (!takeSensor()) return;
    for (auto &kv : *s_sensors) ids.emplace_back(kv.first.c_str());
    xSemaphoreGive(s_mutex);
}

std::string SensorRegistry::getDisplayName(const PsramString &id) {
    if (!s_sensors) return std::string(id.c_str());
    std::string name;
    if (!takeSensor()) return std::string(id.c_str());
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) {
        if (!it->second.nameOverride.empty()) name.assign(it->second.nameOverride.c_str());
        else if (!it->second.autoName.empty()) name.assign(it->second.autoName.c_str());
        else if (!it->second.manufacturer.empty())
            name = std::string(it->second.manufacturer.c_str()) + " " + id.c_str();
    }
    xSemaphoreGive(s_mutex);
    return name.empty() ? std::string(id.c_str()) : name;
}

void SensorRegistry::setManufacturer(const PsramString &id, const PsramString &mfg) {
    if (!s_sensors || mfg.empty()) return;
    if (!takeSensor()) return;
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) it->second.manufacturer = mfg;
    xSemaphoreGive(s_mutex);
}

void SensorRegistry::setRssi(const PsramString &id, int dbm) {
    if (!s_sensors) return;
    if (!takeSensor()) return;
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) {
        // data timestamp untouched: staleness follows T/H/B data, not RSSI
        it->second.reading.hasRssi = true;
        it->second.reading.rssiDbm = dbm;
    }
    xSemaphoreGive(s_mutex);
}

void SensorRegistry::setDisplayName(const PsramString &id, const PsramString &name) {
    if (!s_sensors) return;
    if (!takeSensor()) return;
    (*s_sensors)[id].nameOverride = name;
    xSemaphoreGive(s_mutex);
}

void SensorRegistry::setAutoName(const PsramString &id, const PsramString &name,
                                  bool onlyIfEmpty) {
    if (!s_sensors || name.empty()) return;
    if (!takeSensor()) return;
    auto &entry = (*s_sensors)[id];
    if (!onlyIfEmpty || entry.autoName.empty()) entry.autoName = name;
    xSemaphoreGive(s_mutex);
}

void SensorRegistry::setDeviceMember(const PsramString &id, const PsramString &device,
                                     const PsramString &entity) {
    if (!s_sensors) return;
    if (!takeSensor()) return;
    auto &entry = (*s_sensors)[id];
    entry.device = device;
    bool known = false;
    for (auto &m : entry.members) if (m == entity) { known = true; break; }
    if (!known && !entity.empty()) entry.members.push_back(entity);
    xSemaphoreGive(s_mutex);
}

bool SensorRegistry::getDeviceInfo(const PsramString &id, std::string &device,
                                   std::vector<std::string> &members) {
    if (!s_sensors) return false;
    bool found = false;
    if (!takeSensor()) return false;
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) {
        device.assign(it->second.device.c_str());
        members.clear();
        for (auto &m : it->second.members) members.emplace_back(m.c_str());
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void SensorRegistry::removeDisplayName(const PsramString &id) {
    if (!s_sensors) return;
    if (!takeSensor()) return;
    auto it = s_sensors->find(id);
    if (it != s_sensors->end()) it->second.nameOverride.clear();
    xSemaphoreGive(s_mutex);
}

size_t SensorRegistry::count() {
    if (!s_sensors) return 0;
    if (!takeSensor()) return 0;
    size_t n = s_sensors->size();
    xSemaphoreGive(s_mutex);
    return n;
}

void SensorRegistry::pruneStale(uint32_t maxAgeMillis) {
    if (!s_sensors) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!takeSensor()) return;
    for (auto it = s_sensors->begin(); it != s_sensors->end();) {
        if (now - it->second.reading.lastUpdateMillis > maxAgeMillis) {
            it = s_sensors->erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(s_mutex);
}