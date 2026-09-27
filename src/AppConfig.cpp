#include "AppConfig.hpp"
#include "LogBuffer.hpp"

#include "esp_littlefs.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <new>

static AppConfig *s_cache = nullptr;
static bool s_cacheValid = false;

bool ConfigManager::initFS() {
    static bool done = false;
    if (done) return true;

    static bool cjsonHooksSet = false;
    if (!cjsonHooksSet) {
        cJSON_Hooks hooks;
        // JSON never touches DMA: prefer PSRAM, keep 8-bit access.
        hooks.malloc_fn = [](size_t sz) -> void * { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
        hooks.free_fn = [](void *p) { heap_caps_free(p); };
        cJSON_InitHooks(&hooks);
        cjsonHooksSet = true;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
        .grow_on_mount = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        LogBuffer::logf("LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        LogBuffer::logf("LittleFS: %d bytes total, %d used", total, used);
    }
    done = true;
    return true;
}

AppConfig ConfigManager::defaults() {
    AppConfig cfg;
    cfg.grid_rows = 2;
    cfg.grid_cols = 3;
    return cfg;
}

void ConfigManager::invalidateCache() {
    s_cacheValid = false;
    if (s_cache) {
        s_cache->~AppConfig();
        heap_caps_free(s_cache);
        s_cache = nullptr;
    }
}

bool ConfigManager::load(AppConfig &config) {
    if (s_cacheValid && s_cache) {
        config = *s_cache;
        return config.valid;
    }
    if (!s_cache) {
        void *mem = heap_caps_malloc(sizeof(AppConfig), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem) {
            s_cache = new (mem) AppConfig();
        } else {
            s_cache = new AppConfig();
        }
    }

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config.valid = false;
        s_cache->valid = false;
        s_cacheValid = true;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        config.valid = false;
        s_cache->valid = false;
        s_cacheValid = true;
        return false;
    }
    std::vector<char> data(len + 1);
    if (fread(data.data(), 1, len, f) != (size_t)len) {
        fclose(f);
        config.valid = false;
        s_cache->valid = false;
        s_cacheValid = true;
        return false;
    }
    data[len] = '\0';
    fclose(f);

    cJSON *json = cJSON_Parse(data.data());
    if (!json) {
        config.valid = false;
        s_cache->valid = false;
        s_cacheValid = true;
        return false;
    }

    auto getStr = [json](const char *key) -> std::string {
        cJSON *it = cJSON_GetObjectItem(json, key);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
        return {};
    };
    auto getInt = [json](const char *key, int def) -> int {
        cJSON *it = cJSON_GetObjectItem(json, key);
        return cJSON_IsNumber(it) ? it->valueint : def;
    };
    auto getBool = [json](const char *key, bool def) -> bool {
        cJSON *it = cJSON_GetObjectItem(json, key);
        return it ? (cJSON_IsTrue(it) ? true : cJSON_IsFalse(it) ? false : def) : def;
    };
    auto getArrayStr = [json](const char *key) -> std::vector<std::string> {
        std::vector<std::string> out;
        cJSON *arr = cJSON_GetObjectItem(json, key);
        if (cJSON_IsArray(arr)) {
            cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (cJSON_IsString(it) && it->valuestring) out.emplace_back(it->valuestring);
                else out.emplace_back();
            }
        }
        return out;
    };

    auto *cfg = s_cache;
    *cfg = AppConfig();
    cfg->wifi_ssid = getStr("wifi_ssid");
    cfg->wifi_password = getStr("wifi_password");
    cfg->wifi_ssid2 = getStr("wifi_ssid2");
    cfg->wifi_password2 = getStr("wifi_password2");
    cfg->use_dhcp = getBool("use_dhcp", true);
    cfg->static_ip = getStr("static_ip");
    cfg->static_netmask = getStr("static_netmask");
    cfg->static_gateway = getStr("static_gateway");
    cfg->hotspot_on_fail = getBool("hotspot_on_fail", true);
    cfg->username = getStr("username");
    cfg->password = getStr("password");
    if (cfg->username.empty()) cfg->username = "admin";
    if (cfg->password.empty()) cfg->password = "admin";

    cfg->data_source = getInt("data_source", (int)DataSourceType::OFF);
    // NOTE: OFF is 0 (the minimum), not the maximum - validate against the
    // full 0..SHELLY range. A "> OFF" check here silently clamped every valid
    // source back to OFF on every load.
    if (cfg->data_source < (int)DataSourceType::OFF ||
        cfg->data_source > (int)DataSourceType::SHELLY) {
        cfg->data_source = (int)DataSourceType::OFF;
    }

    cfg->ha_url = getStr("ha_url");
    cfg->ha_token = getStr("ha_token");

    cfg->mqtt_host = getStr("mqtt_host");
    cfg->mqtt_port = getInt("mqtt_port", 1883);
    cfg->mqtt_username = getStr("mqtt_username");
    cfg->mqtt_password = getStr("mqtt_password");
    cfg->mqtt_int_host = getStr("mqtt_int_host");
    cfg->mqtt_int_port = getInt("mqtt_int_port", 1883);
    cfg->mqtt_int_username = getStr("mqtt_int_username");
    cfg->mqtt_int_password = getStr("mqtt_int_password");

    cfg->grid_rows = getInt("grid_rows", 2);
    cfg->grid_cols = getInt("grid_cols", 3);
    if (cfg->grid_rows < 1) cfg->grid_rows = 1;
    if (cfg->grid_rows > 3) cfg->grid_rows = 3;
    if (cfg->grid_cols < 1) cfg->grid_cols = 1;
    if (cfg->grid_cols > 4) cfg->grid_cols = 4;

    cfg->cells = getArrayStr("cells");
    cfg->name_override_ids = getArrayStr("name_override_ids");
    cfg->ntp_server = getStr("ntp_server");
    if (cfg->ntp_server.empty()) cfg->ntp_server = "pool.ntp.org";
    cfg->timezone = getStr("timezone");
    if (cfg->timezone.empty()) cfg->timezone = AppConfig::DEFAULT_TIMEZONE;

    cfg->valid = !cfg->wifi_ssid.empty() || !cfg->wifi_ssid2.empty();
    cJSON_Delete(json);

    s_cacheValid = true;
    config = *s_cache;
    LogBuffer::logf("Config loaded: valid=%d ssid='%s'", config.valid, config.wifi_ssid.c_str());
    return config.valid;
}

bool ConfigManager::save(const AppConfig &config) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "wifi_ssid", config.wifi_ssid.c_str());
    cJSON_AddStringToObject(json, "wifi_password", config.wifi_password.c_str());
    cJSON_AddStringToObject(json, "wifi_ssid2", config.wifi_ssid2.c_str());
    cJSON_AddStringToObject(json, "wifi_password2", config.wifi_password2.c_str());
    cJSON_AddBoolToObject(json, "use_dhcp", config.use_dhcp);
    cJSON_AddStringToObject(json, "static_ip", config.static_ip.c_str());
    cJSON_AddStringToObject(json, "static_netmask", config.static_netmask.c_str());
    cJSON_AddStringToObject(json, "static_gateway", config.static_gateway.c_str());
    cJSON_AddBoolToObject(json, "hotspot_on_fail", config.hotspot_on_fail);
    cJSON_AddStringToObject(json, "username", config.username.c_str());
    cJSON_AddStringToObject(json, "password", config.password.c_str());
    cJSON_AddNumberToObject(json, "data_source", config.data_source);
    cJSON_AddStringToObject(json, "ha_url", config.ha_url.c_str());
    cJSON_AddStringToObject(json, "ha_token", config.ha_token.c_str());
    cJSON_AddStringToObject(json, "mqtt_host", config.mqtt_host.c_str());
    cJSON_AddNumberToObject(json, "mqtt_port", config.mqtt_port);
    cJSON_AddStringToObject(json, "mqtt_username", config.mqtt_username.c_str());
    cJSON_AddStringToObject(json, "mqtt_password", config.mqtt_password.c_str());
    cJSON_AddStringToObject(json, "mqtt_int_host", config.mqtt_int_host.c_str());
    cJSON_AddNumberToObject(json, "mqtt_int_port", config.mqtt_int_port);
    cJSON_AddStringToObject(json, "mqtt_int_username", config.mqtt_int_username.c_str());
    cJSON_AddStringToObject(json, "mqtt_int_password", config.mqtt_int_password.c_str());
    cJSON_AddNumberToObject(json, "grid_rows", config.grid_rows);
    cJSON_AddNumberToObject(json, "grid_cols", config.grid_cols);

    cJSON *cellArr = cJSON_AddArrayToObject(json, "cells");
    for (auto &c : config.cells) cJSON_AddItemToArray(cellArr, cJSON_CreateString(c.c_str()));
    cJSON *nameArr = cJSON_AddArrayToObject(json, "name_override_ids");
    for (auto &n : config.name_override_ids) cJSON_AddItemToArray(nameArr, cJSON_CreateString(n.c_str()));

    cJSON_AddStringToObject(json, "ntp_server", config.ntp_server.c_str());
    cJSON_AddStringToObject(json, "timezone", config.timezone.c_str());

    char *data = cJSON_Print(json);
    cJSON_Delete(json);
    if (!data) return false;

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        heap_caps_free(data);
        return false;
    }
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    heap_caps_free(data);

    LogBuffer::logf("Config saved");
    invalidateCache();
    return true;
}

bool ConfigManager::erase() {
    invalidateCache();
    return remove(CONFIG_PATH) == 0;
}