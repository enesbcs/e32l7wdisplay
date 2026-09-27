#pragma once

#include <string>
#include <map>
#include <vector>
#include <utility>
#include <new>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// PSRAM allocator + string/vector aliases. Plain std::string char buffers
// come from the default (internal!) heap, which once held ~50KB of HA
// entity/device strings and starved the web server. All registry storage
// uses these; the public API keeps std::string (explicit c_str() converts
// at the boundary) so callers are untouched.
template <class T>
struct PsramAllocator {
    using value_type = T;
    T *allocate(std::size_t n) {
        return (T *)heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(T *p, std::size_t) noexcept {
        heap_caps_free(p);
    }
    template <class U, class... Args>
    void construct(U *p, Args &&...args) {
        new (p) U(std::forward<Args>(args)...);
    }
    template <class U>
    void destroy(U *p) {
        p->~U();
    }
    template <class U>
    PsramAllocator(const PsramAllocator<U> &) {}
    PsramAllocator() {}
};
// stateless: all instances compare equal (required by basic_string)
template <class T, class U>
bool operator==(const PsramAllocator<T> &, const PsramAllocator<U> &) { return true; }
template <class T, class U>
bool operator!=(const PsramAllocator<T> &, const PsramAllocator<U> &) { return false; }

// basic_string has no cross-allocator copy ctor, so a converting subclass:
// implicit construction from std::string / const char* keeps every call
// site compiling unchanged; storage lands in PSRAM.
struct PsramString : public std::basic_string<char, std::char_traits<char>, PsramAllocator<char>> {
    using Base = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
    PsramString() : Base() {}
    PsramString(const char *s) : Base(s ? s : "") {}
    PsramString(const char *s, std::size_t n) : Base(s, n) {}
    PsramString(const std::string &s) : Base(s.c_str(), s.size()) {}
    PsramString(const PsramString &o) : Base(o) {}
    PsramString(const Base &o) : Base(o) {}
    PsramString &operator=(const PsramString &o) { Base::operator=(o); return *this; }
};

template <class T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

struct SensorReading {
    bool hasTemperature = false;
    float temperature = 0;
    bool hasHumidity = false;
    float humidity = 0;
    bool hasBattery = false;
    float battery = 0; // percent 0..100
    bool hasRssi = false;
    int rssiDbm = 0;
    uint32_t lastUpdateMillis = 0; // millis() timestamp
    // bumped only when a stored value actually changes (upsert compares);
    // lets readers skip unchanged entries without string work
    uint32_t generation = 0;
    // last accepted RSSI write (throttle: min interval + min delta)
    uint32_t lastRssiMs = 0;
};

// One-lock snapshot: reading + resolved display name. Replaces the
// get()+getDisplayName() pair (two locks + two string builds per cell).
struct SensorSnapshot {
    bool found = false;
    SensorReading reading;
    std::string name;
};

// Sensor registry shared by all data sources (HA / Shelly / BLE).
// Thread-safe. Sensor id rules:
//   Home Assistant:  "ha:<device_id>" (entities grouped by device_id,
//                    fallback "ha:<entity_id>")
//   Shelly MQTT:     "shelly.<device-id>"           (temp+humidity+battery)
//   BLE:             "<mac:lower,no-colons>"
class SensorRegistry {
public:
    static bool init();

    // NOTE: ids/names take PsramString so their char buffers live in PSRAM
    // (plain std::string would malloc them from internal RAM). Callers pass
    // std::string / const char* unchanged via implicit conversion.
    static void upsert(const PsramString &id, bool hasT, float t, bool hasH, float h,
                       bool hasB, float b);
    // manufacturer tag for BLE sensors ("Xiaomi", "BTHome", "ATC"); shown
    // before the MAC in lists unless the user set a name override.
    static void setManufacturer(const PsramString &id, const PsramString &mfg);
    // RSSI refresh (e.g. every BLE advert); updates existing entries only.
    static void setRssi(const PsramString &id, int dbm);
    static bool get(const PsramString &id, SensorReading &out);
    static uint32_t getGeneration(const PsramString &id);
    static bool getSnapshot(const PsramString &id, SensorSnapshot &out);
    static void getAllCells(std::vector<std::string> &ids);

    // display name helpers (empty = auto name).
    // Precedence: user override > auto-name (HA: temperature member's
    // entity_id with the "sensor." prefix cut) > BLE "manufacturer id" > id.
    static std::string getDisplayName(const PsramString &id);
    static void setDisplayName(const PsramString &id, const PsramString &name);
    static void removeDisplayName(const PsramString &id);
    // device auto-name (HA); onlyIfEmpty keeps the first (temperature) one
    static void setAutoName(const PsramString &id, const PsramString &name,
                            bool onlyIfEmpty = false);
    // device grouping: raw HA device_id ("") + member entity_ids
    static void setDeviceMember(const PsramString &id, const PsramString &device,
                                const PsramString &entity);
    static bool getDeviceInfo(const PsramString &id, std::string &device,
                              std::vector<std::string> &members);

    static size_t count();
    static void pruneStale(uint32_t maxAgeMillis);
};