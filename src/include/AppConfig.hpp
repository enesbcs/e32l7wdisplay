#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Data source selection, values stored in settings
enum class DataSourceType : int {
    OFF = 0,     // no source: everything stopped (heap/wifi test)
    BLE = 1,     // passive BLE (ATC/ATC1441, Xiaomi, BTHome)
    HOMEASSISTANT = 2, // HA WebSocket (ws://)
    SHELLY = 3,   // Shelly MQTT (Gen1+Gen2)
};

struct AppConfig {
    // --- WiFi ---
    std::string wifi_ssid;
    std::string wifi_password;
    std::string wifi_ssid2;
    std::string wifi_password2;
    bool use_dhcp = true;
    std::string static_ip;
    std::string static_netmask;
    std::string static_gateway;
    bool hotspot_on_fail = true;

    // --- webUI credentials ---
    std::string username = "admin";
    std::string password = "admin";

    // --- data source ---
    int data_source = (int)DataSourceType::OFF;

    // --- Home Assistant ---
    std::string ha_url;    // ws://host:8123/api/websocket (plain ws:// LAN only)
    std::string ha_token;

    // --- Shelly MQTT ---
    std::string mqtt_host;
    int mqtt_port = 1883;
    std::string mqtt_username;
    std::string mqtt_password;

    // --- Integration MQTT (HA autodiscovery) ---
    // Empty host = integration off. Topic prefix is derived (AP name,
    // esp32-xxxxxx), never stored. Shares the Shelly client when host /
    // port / user / pass all match AND the Shelly source is active.
    std::string mqtt_int_host;
    int mqtt_int_port = 1883;
    std::string mqtt_int_username;
    std::string mqtt_int_password;

    // --- Display / dashboard ---
    int grid_rows = 2;
    int grid_cols = 3;
    // one sensor id per cell, "" = empty cell; order: row-major
    std::vector<std::string> cells;
    // display name overrides: sensor id -> name  ("" removes override)
    std::vector<std::string> name_override_ids;

    // --- NTP ---
    std::string ntp_server = "pool.ntp.org";
    // POSIX TZ string (WebUI Settings); empty = DEFAULT_TIMEZONE
    std::string timezone;
    static constexpr const char *DEFAULT_TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

    bool valid = false;
};

class ConfigManager {
public:
    // Mount the shared (Tasmota-compatible LittleFS) filesystem.
    // If it cannot be mounted as LittleFS, it is formatted (Tasmota uses
    // LittleFS, so files are preserved).
    static bool initFS();
    static bool load(AppConfig &config);
    static bool save(const AppConfig &config);
    static bool erase();
    static void invalidateCache();

    static AppConfig defaults();

    static constexpr const char *CONFIG_PATH = "/spiffs/config.json";
    static constexpr const char *PARTITION_LABEL = "spiffs";
};

// Live global config owned by main.cpp. /api/save MUST update it, otherwise
// background tasks (WiFi reconnect re-apply) keep using the stale boot-time
// copy and silently revert the source + wipe the cells.
void updateLiveConfig(const AppConfig &cfg);
// Apply a POSIX TZ string process-wide (empty = DEFAULT_TIMEZONE).
// Defined in main.cpp next to ntpInit (setenv/tzset belong together).
void applyTimezone(const std::string &tz);
// Snapshot of the currently running config (for change detection).
AppConfig getLiveConfig();