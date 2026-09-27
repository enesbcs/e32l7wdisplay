#include "WebServer.hpp"
#include "WiFiManager.hpp"
#include "AppConfig.hpp"
#include "DataSource.hpp"
#include "Dashboard.hpp"
#include "HAWebSocket.hpp"
#include "ShellyMQTT.hpp"
#include "Integration.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"
#include "PinConfig.hpp"
#include "Version.hpp"

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include <ctime>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

extern "C" const char WEBUI_HTML[];

static httpd_handle_t s_server = nullptr;

// ------------------------------------------------------------------ helpers
static bool checkAuth(httpd_req_t *req) {
    // In AP/setup mode the root page is the captive portal page that the phone
    // opens first (no credentials yet) - never 401 there steals still while
    // iOS/Android captive-detection hits /generate_204 then lands on "/".
    if (WiFiManager::getMode() == WiFiManager::Mode::HOTSPOT) return true;
    AppConfig cfg;
    if (!ConfigManager::load(cfg)) return true; // no config -> allow (first setup)
    size_t buflen = 0;
    httpd_req_get_hdr_value_len(req, "Authorization");
    char buf[256];
    buflen = sizeof(buf);
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buflen) != ESP_OK) return false;
    const char *prefix = "Basic ";
    if (strncmp(buf, prefix, 6) != 0) return false;
    // base64 decode
    const char *b64 = buf + 6;
    size_t b64len = strlen(b64);

    static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char userpass[128];
    size_t up = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < b64len && up < sizeof(userpass) - 1; i++) {
        if (b64[i] == '=') break;
        const char *pos = strchr(b64chars, b64[i]);
        if (!pos) continue;
        acc = (acc << 6) | (int)(pos - b64chars);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            userpass[up++] = (char)((acc >> bits) & 0xFF);
        }
    }
    userpass[up] = '\0';

    char expect[128];
    snprintf(expect, sizeof(expect), "%s:%s", cfg.username.c_str(), cfg.password.c_str());
    if (strcmp(userpass, expect) != 0) {
        // tarpit: a failed login stalls the attacker ~500ms (brute force
        // divided by a thousand), legitimate typo retries barely notice it
        vTaskDelay(pdMS_TO_TICKS(500));
        return false;
    }
    return true;
}

static void resp401(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"display\"");
    httpd_resp_send(req, "auth required", HTTPD_RESP_USE_STRLEN);
}

// read whole request body (bounded, PSRAM)
static char *readBody(httpd_req_t *req, size_t maxLen, size_t *outLen) {
    size_t total = req->content_len;
    if (total == 0) { *outLen = 0; return nullptr; }
    if (total > maxLen) total = maxLen;
    char *buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(total + 1);
    if (!buf) return nullptr;
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = '\0';
    *outLen = got;
    return buf;
}

static void sendJson(httpd_req_t *req, const std::string &json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
}

// -------------------------------------------------------------------- routes
static std::string statusJson() {
    AppConfig cfg;
    ConfigManager::load(cfg);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_mode", WiFiManager::getMode() == WiFiManager::Mode::CLIENT ? "client"
        : WiFiManager::getMode() == WiFiManager::Mode::HOTSPOT ? "ap" : "connecting");
    cJSON_AddStringToObject(j, "wifi_ssid", WiFiManager::s_lastSSID.c_str());
    cJSON_AddStringToObject(j, "ip", WiFiManager::getIP().c_str());
    cJSON_AddStringToObject(j, "mac", WiFiManager::getMAC().c_str());
    cJSON_AddStringToObject(j, "ap_name", WiFiManager::getAPName().c_str());
    const char *srcName = cfg.data_source == (int)DataSourceType::HOMEASSISTANT ? "ha"
        : cfg.data_source == (int)DataSourceType::SHELLY ? "shelly"
        : cfg.data_source == (int)DataSourceType::OFF ? "off" : "ble";
    cJSON_AddStringToObject(j, "data_source", srcName);
    cJSON_AddStringToObject(j, "fw_version", APP_FW_VERSION);
    cJSON_AddNumberToObject(j, "sensor_count", (double)SensorRegistry::count());
    // STA RSSI for the mirror wifi fan (null when not STA-connected)
    {
        int rssiDbm = 0;
        bool haveRssi = false;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ipi = {};
        if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr != 0
            && esp_wifi_sta_get_rssi(&rssiDbm) == ESP_OK) {
            haveRssi = true;
        }
        if (haveRssi) cJSON_AddNumberToObject(j, "wifi_rssi", (double)rssiDbm);
        else cJSON_AddNullToObject(j, "wifi_rssi");
    }
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    std::string out(s);
    cJSON_free(s);
    return out;
}

static esp_err_t handleRoot(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, WEBUI_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handleApiStatus(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    sendJson(req, statusJson());
    return ESP_OK;
}

// GET /api/info - device info for the Info tab: CPU, SRAM/PSRAM totals,
// flash size, partition layout, firmware version, first-setup flag.
static esp_err_t handleApiInfo(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    AppConfig cfg;
    bool hasCfg = ConfigManager::load(cfg);
    cJSON *j = cJSON_CreateObject();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "ESP32";
    if (chip.model == CHIP_ESP32S3) model = "ESP32-S3";
    else if (chip.model == CHIP_ESP32S2) model = "ESP32-S2";
    else if (chip.model == CHIP_ESP32C3) model = "ESP32-C3";
    else if (chip.model == CHIP_ESP32C6) model = "ESP32-C6";
    else if (chip.model == CHIP_ESP32H2) model = "ESP32-H2";
    else if (chip.model == CHIP_ESP32P4) model = "ESP32-P4";
    char cpu[64];
    snprintf(cpu, sizeof(cpu), "%s rev%d, %d cores @ %d MHz", model,
             chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddStringToObject(j, "cpu", cpu);
    cJSON_AddNumberToObject(j, "sram_total",
        (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "sram_free",
        (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "psram_total",
        (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "psram_free",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK) flashSize = 0;
    cJSON_AddNumberToObject(j, "flash_size", (double)flashSize);
    cJSON_AddStringToObject(j, "fw_version", APP_FW_VERSION);
    cJSON_AddBoolToObject(j, "configured", hasCfg && cfg.valid);
    float mcuTemp = 0;
    if (Dashboard::readMcuTempC(mcuTemp)) cJSON_AddNumberToObject(j, "mcu_temp", (double)mcuTemp);
    else cJSON_AddNullToObject(j, "mcu_temp");
    cJSON_AddNumberToObject(j, "uptime_s",
        (double)(esp_timer_get_time() / 1000000ULL));
    // wall clock, same validity rule as the dashboard status bar
    char dt[20] = {0};
    bool timeOk = false;
    time_t nowT = time(nullptr);
    if (nowT > 1700000000L) {
        struct tm tmv;
        int Y, M, D, h, m;
        if (localtime_r(&nowT, &tmv)
            && (Y = tmv.tm_year + 1900) >= 2024 && Y <= 2100
            && (M = tmv.tm_mon + 1) >= 1 && M <= 12
            && (D = tmv.tm_mday) >= 1 && D <= 31
            && (h = tmv.tm_hour) >= 0 && h < 24
            && (m = tmv.tm_min) >= 0 && m < 60) {
            snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d", Y, M, D, h, m);
            timeOk = true;
        }
    }
    cJSON_AddStringToObject(j, "datetime", dt);
    cJSON_AddBoolToObject(j, "time_ok", timeOk);
    cJSON *parts = cJSON_AddArrayToObject(j, "partitions");
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "label", p->label);
        cJSON_AddNumberToObject(e, "type", p->type);
        cJSON_AddNumberToObject(e, "subtype", p->subtype);
        cJSON_AddNumberToObject(e, "offset", (double)p->address);
        cJSON_AddNumberToObject(e, "size", (double)p->size);
        cJSON_AddItemToArray(parts, e);
    }
    esp_partition_iterator_release(it);
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    sendJson(req, s);
    cJSON_free(s);
    return ESP_OK;
}

static esp_err_t handleApiConfig(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    AppConfig cfg;
    ConfigManager::load(cfg);
    cJSON *j = cJSON_Parse(statusJson().c_str());

    // secrets are masked: "********" when set, "" when unset - the real
    // values never leave the device, and the UI never sends secrets back
    // unless the user typed into the field (untouched = key omitted).
    auto masked = [](const std::string &v) { return v.empty() ? "" : "********"; };
    cJSON_AddStringToObject(j, "wifi_ssid", cfg.wifi_ssid.c_str());
    cJSON_AddStringToObject(j, "wifi_password", masked(cfg.wifi_password));
    cJSON_AddStringToObject(j, "wifi_ssid2", cfg.wifi_ssid2.c_str());
    cJSON_AddStringToObject(j, "wifi_password2", masked(cfg.wifi_password2));
    cJSON_AddBoolToObject(j, "use_dhcp", cfg.use_dhcp);
    cJSON_AddStringToObject(j, "static_ip", cfg.static_ip.c_str());
    cJSON_AddStringToObject(j, "static_netmask", cfg.static_netmask.c_str());
    cJSON_AddStringToObject(j, "static_gateway", cfg.static_gateway.c_str());
    cJSON_AddBoolToObject(j, "hotspot_on_fail", cfg.hotspot_on_fail);
    cJSON_AddNumberToObject(j, "grid_rows", cfg.grid_rows);
    cJSON_AddNumberToObject(j, "grid_cols", cfg.grid_cols);
    cJSON_AddStringToObject(j, "ha_url", cfg.ha_url.c_str());
    cJSON_AddStringToObject(j, "ha_token", masked(cfg.ha_token));
    cJSON_AddStringToObject(j, "mqtt_host", cfg.mqtt_host.c_str());
    cJSON_AddNumberToObject(j, "mqtt_port", cfg.mqtt_port);
    cJSON_AddStringToObject(j, "mqtt_username", cfg.mqtt_username.c_str());
    cJSON_AddStringToObject(j, "mqtt_password", masked(cfg.mqtt_password));
    cJSON_AddStringToObject(j, "mqtt_int_host", cfg.mqtt_int_host.c_str());
    cJSON_AddNumberToObject(j, "mqtt_int_port", cfg.mqtt_int_port);
    cJSON_AddStringToObject(j, "mqtt_int_username", cfg.mqtt_int_username.c_str());
    cJSON_AddStringToObject(j, "mqtt_int_password", masked(cfg.mqtt_int_password));
    cJSON_AddStringToObject(j, "ntp_server", cfg.ntp_server.c_str());
    cJSON_AddStringToObject(j, "timezone", cfg.timezone.c_str());
    cJSON_AddStringToObject(j, "username", cfg.username.c_str());
    cJSON_AddStringToObject(j, "password", masked(cfg.password));
    // cell mapping: array of {id, name}
    cJSON *cells = cJSON_AddArrayToObject(j, "cells");
    size_t N = cfg.grid_rows * cfg.grid_cols;
    for (size_t i = 0; i < N; i++) {
        cJSON *cell = cJSON_CreateObject();
        std::string id = i < cfg.cells.size() ? cfg.cells[i] : "";
        cJSON_AddStringToObject(cell, "id", id.c_str());
        std::string name = id.empty() ? "" : SensorRegistry::getDisplayName(id);
        cJSON_AddStringToObject(cell, "name", name.c_str());
        cJSON_AddItemToArray(cells, cell);
    }
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    sendJson(req, s);
    cJSON_free(s);
    return ESP_OK;
}

// apply raw cell strings to config.cells (row-major)
static void setCells(AppConfig &cfg, const cJSON *arr) {
    cfg.cells.clear();
    const cJSON *el;
    cJSON_ArrayForEach(el, arr) {
        if (cJSON_IsObject(el)) {
            const cJSON *id = cJSON_GetObjectItem(el, "id");
            cfg.cells.push_back(id && cJSON_IsString(id) ? id->valuestring : "");
        } else if (cJSON_IsString(el)) {
            cfg.cells.push_back(el->valuestring);
        }
    }
    // assignment always stores the full 3x4=12 rows; the display renders the
    // first grid_rows*grid_cols of them (extras survive grid shrink/grow)
    while (cfg.cells.size() < 12) cfg.cells.push_back("");
    if (cfg.cells.size() > 12) cfg.cells.resize(12);
}

static void applyNames(AppConfig &cfg, const cJSON *arr) {
    // persist overrides index-aligned with cells as "id\x1fname" pairs so a
    // reboot (Dashboard::applyConfig) can restore them to the registry.
    cfg.name_override_ids.clear();
    cfg.name_override_ids.resize(cfg.cells.size());
    const cJSON *el;
    int i = 0;
    cJSON_ArrayForEach(el, arr) {
        std::string id = cfg.cells.size() > (size_t)i ? cfg.cells[i] : "";
        std::string nm;
        if (cJSON_IsObject(el)) {
            const cJSON *name = cJSON_GetObjectItem(el, "name");
            if (name && cJSON_IsString(name) && name->valuestring) nm = name->valuestring;
            // display-only: strip markup + cap 48 (XSS defense in depth)
            std::string o;
            for (char c : nm) {
                if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'')
                    continue;
                o += c;
            }
            if (o.size() > 48) o.resize(48);
            nm.swap(o);
        }
        if (!id.empty() && !nm.empty()) {
            SensorRegistry::setDisplayName(id, nm);
            if ((size_t)i < cfg.name_override_ids.size())
                cfg.name_override_ids[i] = id + '\x1f' + nm;
        } else if (!id.empty()) {
            SensorRegistry::removeDisplayName(id);
        }
        i++;
    }
}

static esp_err_t handleApiSave(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    size_t len;
    char *body = readBody(req, 16384, &len);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }

    cJSON *j = cJSON_Parse(body);
    heap_caps_free(body);
    if (!j) { httpd_resp_send_500(req); return ESP_OK; }

    AppConfig cfg;
    ConfigManager::load(cfg);

    auto getStrSave = [&](const char *k, std::string &dst) {
        const cJSON *it = cJSON_GetObjectItem(j, k);
        if (it && cJSON_IsString(it) && strcmp(it->valuestring, "********") != 0) dst = it->valuestring;
    };
    // Secrets (passwords/token): applied ONLY when a real new value arrives.
    // Absent keys, the "********" mask, and empty strings all mean "keep the
    // stored value" - the WebUI omits untouched secret fields outright, so a
    // stale page, autofill, or a mask round-trip can never wipe them.
    // NOTE: values are logged by LENGTH only, never content.
    auto getSecretSave = [&](const char *name, const char *k, std::string &dst) {
        const cJSON *it = cJSON_GetObjectItem(j, k);
        if (!it || !cJSON_IsString(it) || !it->valuestring) return;
        if (strcmp(it->valuestring, "********") == 0 || it->valuestring[0] == '\0') {
            LogBuffer::logf("save: %s kept (len %d)", name, (int)dst.size());
            return;
        }
        dst = it->valuestring;
        LogBuffer::logf("save: %s replaced (len %d)", name, (int)dst.size());
    };
    auto getInt = [&](const char *k, int def) {
        const cJSON *it = cJSON_GetObjectItem(j, k);
        return (it && cJSON_IsNumber(it)) ? it->valueint : def;
    };
    auto getBool = [&](const char *k, bool def) {
        const cJSON *it = cJSON_GetObjectItem(j, k);
        return it ? (cJSON_IsTrue(it) ? true : cJSON_IsFalse(it) ? false : def) : def;
    };

    getStrSave("wifi_ssid", cfg.wifi_ssid);
    getSecretSave("wifi_password", "wifi_password", cfg.wifi_password);
    getStrSave("wifi_ssid2", cfg.wifi_ssid2);
    getSecretSave("wifi_password2", "wifi_password2", cfg.wifi_password2);
    cfg.use_dhcp = getBool("use_dhcp", cfg.use_dhcp);
    getStrSave("static_ip", cfg.static_ip);
    getStrSave("static_netmask", cfg.static_netmask);
    getStrSave("static_gateway", cfg.static_gateway);
    cfg.hotspot_on_fail = getBool("hotspot_on_fail", cfg.hotspot_on_fail);
    getStrSave("ha_url", cfg.ha_url);
    getSecretSave("ha_token", "ha_token", cfg.ha_token);
    getStrSave("mqtt_host", cfg.mqtt_host);
    cfg.mqtt_port = getInt("mqtt_port", cfg.mqtt_port);
    getStrSave("mqtt_username", cfg.mqtt_username);
    getSecretSave("mqtt_password", "mqtt_password", cfg.mqtt_password);
    getStrSave("mqtt_int_host", cfg.mqtt_int_host);
    cfg.mqtt_int_port = getInt("mqtt_int_port", cfg.mqtt_int_port);
    getStrSave("mqtt_int_username", cfg.mqtt_int_username);
    getSecretSave("mqtt_int_password", "mqtt_int_password", cfg.mqtt_int_password);
    getStrSave("ntp_server", cfg.ntp_server);
    getStrSave("timezone", cfg.timezone);
    // live TZ apply only when the Settings form actually sent it (grid/wifi
    // saves carry no timezone key; setenv/tzset touch process-global state)
    if (cJSON_GetObjectItem(j, "timezone") != nullptr) applyTimezone(cfg.timezone);
    getStrSave("username", cfg.username);
    getSecretSave("web password", "password", cfg.password);

    // field length caps (silent truncation is accepted policy: oversized
    // input can never bloat LittleFS/NVS/heap - and overlong credentials
    // can never weaken auth by truncation either. ha_token is exempt on
    // purpose: truncating it would break HA auth instead of protecting it.)
    auto cap = [&](std::string &v, size_t n) {
        if (v.size() > n) v.resize(n);
    };
    cap(cfg.username, 32); cap(cfg.password, 64);
    cap(cfg.wifi_ssid, 32); cap(cfg.wifi_password, 64);
    cap(cfg.wifi_ssid2, 32); cap(cfg.wifi_password2, 64);
    cap(cfg.ha_url, 128);
    cap(cfg.mqtt_host, 128); cap(cfg.mqtt_username, 128); cap(cfg.mqtt_password, 64);
    cap(cfg.mqtt_int_host, 128); cap(cfg.mqtt_int_username, 128); cap(cfg.mqtt_int_password, 64);
    cap(cfg.ntp_server, 128); cap(cfg.timezone, 128);

    // save-time XSS strip (defense in depth; output esc() is the main
    // layer). Display names lose <>&"' (display-only); SSIDs lose <> only
    // (real networks may contain &'" - output escaping covers the rest).
    // Usernames additionally lose ':' (illegal in Basic-auth userinfo).
    auto stripTags = [&](std::string &v) {
        std::string o;
        for (char c : v) if (c != '<' && c != '>') o += c;
        v.swap(o);
    };
    auto stripHtml = [&](std::string &v, bool colonToo) {
        std::string o;
        for (char c : v) {
            if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\''
                || (colonToo && c == ':'))
                continue;
            o += c;
        }
        v.swap(o);
    };
    stripTags(cfg.wifi_ssid);
    stripTags(cfg.wifi_ssid2);
    stripHtml(cfg.username, true);
    stripHtml(cfg.mqtt_username, false);

    // static IP format gate (DHCP off + any static field set requires all
    // three as dotted quads with non-zero address/gateway)
    if (!cfg.use_dhcp && (!cfg.static_ip.empty() || !cfg.static_netmask.empty()
            || !cfg.static_gateway.empty())) {
        esp_ip4_addr_t a, m, g;
        bool ok = esp_netif_str_to_ip4(cfg.static_ip.c_str(), &a) == ESP_OK && a.addr != 0
            && esp_netif_str_to_ip4(cfg.static_netmask.c_str(), &m) == ESP_OK
            && esp_netif_str_to_ip4(cfg.static_gateway.c_str(), &g) == ESP_OK && g.addr != 0;
        if (!ok) {
            cJSON_Delete(j);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad static ip\"}");
            return ESP_OK;
        }
    }

    if (cfg.mqtt_port < 1 || cfg.mqtt_port > 65535
        || cfg.mqtt_int_port < 1 || cfg.mqtt_int_port > 65535) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad mqtt port\"}");
        return ESP_OK;
    }

    const cJSON *src = cJSON_GetObjectItem(j, "data_source");
    if (cJSON_IsString(src)) {
        const char *s = src->valuestring;
        if (strcmp(s, "ble") == 0) cfg.data_source = (int)DataSourceType::BLE;
        else if (strcmp(s, "ha") == 0) cfg.data_source = (int)DataSourceType::HOMEASSISTANT;
        else if (strcmp(s, "shelly") == 0) cfg.data_source = (int)DataSourceType::SHELLY;
        else if (strcmp(s, "off") == 0) cfg.data_source = (int)DataSourceType::OFF;
    }

    cfg.grid_rows = getInt("grid_rows", cfg.grid_rows);
    cfg.grid_cols = getInt("grid_cols", cfg.grid_cols);
    if (cfg.grid_rows < 1) cfg.grid_rows = 1;
    if (cfg.grid_rows > 3) cfg.grid_rows = 3;
    if (cfg.grid_cols < 1) cfg.grid_cols = 1;
    if (cfg.grid_cols > 4) cfg.grid_cols = 4;

    const cJSON *cells = cJSON_GetObjectItem(j, "cells");
    if (cJSON_IsArray(cells)) {
        setCells(cfg, cells);
        applyNames(cfg, cells);
    }

    bool ok = ConfigManager::save(cfg);

    const cJSON *reboot = cJSON_GetObjectItem(j, "reboot");
    bool doReboot = reboot ? (cJSON_IsTrue(reboot) || (cJSON_IsNumber(reboot) && reboot->valueint)) : false;

    cJSON_Delete(j);
    if (!ok) { httpd_resp_send_500(req); return ESP_OK; }

    // Re-activating the source stack (BLE controller, sockets...) on every
    // save - even a grid-size-only one - flaps the radios for nothing.
    // Apply only when source-relevant settings actually changed vs live.
    AppConfig live = getLiveConfig();
    bool srcChanged =
        cfg.data_source != live.data_source ||
        cfg.ha_url != live.ha_url || cfg.ha_token != live.ha_token ||
        cfg.mqtt_host != live.mqtt_host || cfg.mqtt_port != live.mqtt_port ||
        cfg.mqtt_username != live.mqtt_username || cfg.mqtt_password != live.mqtt_password;
    if (srcChanged)
        LogBuffer::logf("Source settings changed, re-applying");

    // keep the live global in sync so reconnect re-apply uses fresh values
    updateLiveConfig(cfg);
    // dashboard assignment filter for HA live events (empty = setup phase)
    HAWebSocket::setWatchedCells(cfg.cells);
    // integration re-evaluates transport (shared vs own) on every save
    Integration::apply(cfg);

    if (doReboot) {
        // Reconnect request: reply immediately, then reset. Applying the data
        // source first would stall the response (e.g. BLE controller init while
        // the AP is up) and make it look like the save did nothing.
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
    }

    // apply immediately where safe (no risk of restart)
    // a genuine source-type switch reboots instead: the BLE stack has no
    // teardown path, a live switch would OOM before the reboot (same shape
    // as the reconnect-reboot above: reply first, then reset)
    bool needReboot = false;
    if (srcChanged) needReboot = DataSource::apply(cfg);
    if (needReboot) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
    }
    // skip the full rebuild when nothing render-relevant changed (repeated
    // saves with identical grid/cells/names); the live snapshot above is
    // pre-update, so the comparison is against the running state
    bool gridSame = cfg.grid_rows == live.grid_rows
        && cfg.grid_cols == live.grid_cols && cfg.cells == live.cells
        && cfg.name_override_ids == live.name_override_ids;
    if (!gridSame && !Dashboard::applyConfig(cfg)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"display busy\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// wifi_ap_record_t.ssid is a fixed 32B field with NO guaranteed NUL when
// the OTA SSID is exactly 32 chars (attacker-settable nearby) - copy with
// explicit terminator before treating it as a C string.
static void ssidToStr(char out33[33], const uint8_t ssid32[32]) {
    memcpy(out33, ssid32, 32);
    out33[32] = '\0';
}

static esp_err_t handleWifiScan(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }

    // scan cache (60s): a blocking all-channel scan stalls the single httpd
    // worker for seconds + flaps APSTA; network selection tolerates staleness
    static std::string s_cached;
    static uint32_t s_cachedMs = 0;
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_cached.empty() && nowMs - s_cachedMs < 60000) {
        sendJson(req, s_cached);
        return ESP_OK;
    }

    // A scan needs a station interface. In AP-only (hotspot/setup) mode the
    // scan silently fails, so temporarily run APSTA: the AP stays up (the
    // phone keeps its link) while the temporary STA interface does the scan.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool toggled = false;
    if (mode == WIFI_MODE_AP) {
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) toggled = true;
    }
    if (mode == WIFI_MODE_AP && !toggled) {
        // switching failed - still answer with an empty list
    }

    wifi_scan_config_t scan = {};
    wifi_ap_record_t *ap = nullptr;
    uint16_t count = 0;
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
        if (count > 32) count = 32;
        if (count) ap = (wifi_ap_record_t *)malloc(count * sizeof(wifi_ap_record_t));
        if (ap) esp_wifi_scan_get_ap_records(&count, ap);
    }

    cJSON *j = cJSON_CreateObject();
    cJSON *net = cJSON_AddArrayToObject(j, "networks");
    if (ap) {
        for (uint16_t i = 0; i < count; i++) {
            cJSON *e = cJSON_CreateObject();
            char ssid[33];
            ssidToStr(ssid, ap[i].ssid);
            cJSON_AddStringToObject(e, "ssid", ssid);
            cJSON_AddNumberToObject(e, "rssi", ap[i].rssi);
            cJSON_AddNumberToObject(e, "auth", ap[i].authmode);
            cJSON_AddItemToArray(net, e);
        }
        free(ap);
    }
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    s_cached = s ? s : "";
    s_cachedMs = nowMs;
    sendJson(req, s);
    cJSON_free(s);

    if (toggled) esp_wifi_set_mode(WIFI_MODE_AP);
    return ESP_OK;
}

static esp_err_t handleListSensors(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    // ?all=1: skip the temperature-only filter. The /dashboard mirror needs
    // every row the RGB display renders (humidity-only devices included);
    // the assignment candidate lists keep the filtered default.
    bool all = false;
    if (httpd_req_get_url_query_len(req) > 0) {
        char q[32], v[8];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
            && httpd_query_key_value(q, "all", v, sizeof(v)) == ESP_OK)
            all = (v[0] == '1');
    }
    std::vector<std::string> ids;
    SensorRegistry::getAllCells(ids);
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "sensors");
    for (auto &id : ids) {
        SensorReading r;
        if (!SensorRegistry::get(id, r)) continue;
        // temperature-only: a device without temperature data (battery-
        // or signal-only) has no place on the candidate lists
        if (!all && !r.hasTemperature) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", id.c_str());
        char buf[32];
        if (r.hasTemperature) { snprintf(buf, sizeof(buf), "%.1f C", r.temperature); cJSON_AddStringToObject(e, "temp", buf); }
        if (r.hasHumidity) { snprintf(buf, sizeof(buf), "%d %%", (int)(r.humidity + 0.5f)); cJSON_AddStringToObject(e, "hum", buf); }
        if (r.hasBattery) { snprintf(buf, sizeof(buf), "%d %%", (int)(r.battery + 0.5f)); cJSON_AddStringToObject(e, "batt", buf); }
        if (r.hasRssi) { snprintf(buf, sizeof(buf), "%d dB", r.rssiDbm); cJSON_AddStringToObject(e, "rssi", buf); }
        cJSON_AddStringToObject(e, "name", SensorRegistry::getDisplayName(id).c_str());
        // device grouping: raw HA device_id + member entity_ids (one row =
        // one physical device; filter events by these entity_ids)
        std::string device;
        std::vector<std::string> members;
        if (SensorRegistry::getDeviceInfo(id, device, members)) {
            if (!device.empty()) cJSON_AddStringToObject(e, "device", device.c_str());
            if (!members.empty()) {
                cJSON *arr2 = cJSON_AddArrayToObject(e, "entities");
                for (auto &m : members) cJSON_AddItemToArray(arr2, cJSON_CreateString(m.c_str()));
            }
        }
        uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
        cJSON_AddNumberToObject(e, "age_s", (double)(nowMs >= r.lastUpdateMillis
            ? (nowMs - r.lastUpdateMillis) / 1000 : 0));
        cJSON_AddItemToArray(arr, e);
    }
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    sendJson(req, s);
    cJSON_free(s);
    return ESP_OK;
}

static esp_err_t handleReboot(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

// POST /api/discover - manual re-discovery for the active source:
// Shelly MQTT broadcasts triggers, HA WebSocket reconnects (full
// subscribe->registry->states chain). BLE needs none (always discovering).
static esp_err_t handleDiscover(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    AppConfig live = getLiveConfig();
    const char *src = "off";
    bool ok = true;
    if (live.data_source == (int)DataSourceType::HOMEASSISTANT) {
        src = "ha";
        LogBuffer::logf("Discovery triggered (ha): reconnecting");
        HAWebSocket::stop();
        HAWebSocket::start(live);
    } else if (live.data_source == (int)DataSourceType::SHELLY) {
        src = "shelly";
        LogBuffer::logf("Discovery triggered (shelly): broadcasting");
        ShellyMQTT::discover();
    } else {
        if (live.data_source == (int)DataSourceType::BLE) src = "ble";
        LogBuffer::logf("Discovery triggered (%s): nothing to do", src);
        ok = false;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"source\":\"%s\"}", ok ? "true" : "false", src);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// GET /api/display - display + device status (Basic auth, same credentials)
// POST /api/display - switch display: ?on=1/0/true/false/on/off or JSON {"on":bool}
static esp_err_t handleDisplay(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    if (req->method == HTTP_POST) {
        bool have = false, on = true;
        // ?on= query first
        if (httpd_req_get_url_query_len(req) > 0) {
            char q[32], v[16] = {0};
            if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
                && httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK) {
                for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
                if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on")) {
                    have = true;
                    on = true;
                } else if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off")) {
                    have = true;
                    on = false;
                }
            }
        }
        // then JSON body
        if (!have && req->content_len > 0) {
            size_t len = 0;
            char *body = readBody(req, 512, &len);
            if (body) {
                cJSON *j = cJSON_Parse(body);
                heap_caps_free(body);
                if (j) {
                    const cJSON *o = cJSON_GetObjectItem(j, "on");
                    if (cJSON_IsTrue(o)) { have = true; on = true; }
                    else if (cJSON_IsFalse(o)) { have = true; on = false; }
                    else if (cJSON_IsNumber(o)) { have = true; on = o->valueint != 0; }
                    cJSON_Delete(j);
                }
            }
        }
        if (!have) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"need on\"}");
            return ESP_OK;
        }
        Dashboard::blankDisplay(!on);
        Integration::publishDisplayState();
    }
    cJSON *j = cJSON_Parse(statusJson().c_str());
    if (!j) { httpd_resp_send_500(req); return ESP_OK; }
    cJSON_AddStringToObject(j, "display", Dashboard::isDisplayOn() ? "on" : "off");
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    sendJson(req, s);
    cJSON_free(s);
    return ESP_OK;
}

static esp_err_t handleLog(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    std::string log = LogBuffer::dump();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, log.c_str(), log.size());
    return ESP_OK;
}

// ------------------------------------------------------------- firmware OTA
// Single-slot OTA, Tasmota safeboot architecture (ported from the tasmota-ota
// project). There is only ONE OTA slot (app0 / ota_0); the factory partition
// holds the safeboot recovery firmware. An app NEVER rewrites the partition it
// runs from - the web UI first hands off to the safeboot partition (/u4 with
// u4=fct erases otadata and reboots into factory) and the actual flash write
// happens THERE (into app0), followed by a reboot into app0.
//
//   /up        GET  update form (Tasmota-style transparent handoff page)
//   /u1        GET  URL upgrade (unsupported)
//   /u3        GET  upload result / info
//   /u4        GET  switch boot partition: u4=fct|ota&api=  (Tasmota-compatible)
//   /safeboot  GET  erase otadata + reboot into the safeboot recovery UI
//   /u2        OPTIONS CORS preflight; POST informational (uploads happen in
//                      safeboot, not here)
//   /api/ota/flash POST /safeboot equivalent for the WebUI button
//
// Like Tasmota, the OTA endpoints are unauthenticated (the device only joins
// the LAN the user already authenticated to). The /api/* configuration
// endpoints keep Basic auth.

static bool runningFromFactory() {
    const esp_partition_t *run = esp_ota_get_running_partition();
    return (run && run->type == ESP_PARTITION_TYPE_APP &&
            run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY);
}

// Erase otadata so the bootloader falls back to the factory (safeboot) slot.
static void prepRestartToSafeboot() {
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (ota) {
        esp_partition_erase_range(ota, 0, 0x2000); // 2x 4KB sectors
    }
}

static void sendOtaPage(httpd_req_t *req, const std::string &body) {
    std::string page =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns=%27http://www.w3.org/2000/svg%27 viewBox=%270 0 32 32%27%3E%3Crect width=%2732%27 height=%2732%27 rx=%277%27 fill=%27%2312161c%27/%3E%3Crect x=%279%27 y=%276%27 width=%274%27 height=%2713%27 fill=%27%23ffbe5a%27/%3E%3Ccircle cx=%2711%27 cy=%2723%27 r=%275%27 fill=%27%23ffbe5a%27/%3E%3Cpolygon points=%2722,8 17,20 27,20%27 fill=%27%236ebeFF%27/%3E%3Ccircle cx=%2722%27 cy=%2720%27 r=%275%27 fill=%27%236ebeFF%27/%3E%3C/svg%3E\">"
        "<title>L7 Display - OTA</title><style>"
        "body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:#12161c;color:#e8e8e8;padding:16px}"
        ".card{background:#1b212b;border:1px solid #2a3038;border-radius:8px;padding:16px;max-width:660px;margin:12px auto}"
        "h2{font-size:15px;margin:0 0 10px;color:#9fc0ff}"
        ".btn{display:inline-block;background:#2f6fd0;color:#fff;border:0;border-radius:6px;padding:10px 16px;font-size:14px;font-weight:600;text-decoration:none;cursor:pointer;margin:2px}"
        ".btn.alt{background:#232b38;color:#e8e8e8}"
        "input[type=file]{margin:8px 0}.msg{padding:10px;border-radius:6px;margin:8px 0}"
        ".ok{background:#0d2c15;color:#4ade80}.err{background:#3a0d0d;color:#f87171}"
        ".muted{color:#7f8ba1;font-size:12px}"
        "</style></head><body><div class='card'>" + body + "</div></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page.c_str(), page.size());
}

// GET /up - firmware update form with the transparent safeboot handoff
static esp_err_t handleUpgradePage(httpd_req_t *req) {
    std::string h;
    h += "<h2>Firmware update</h2>";
    h += "<p class='muted'>Select an ESP32 .bin image. The device transparently "
         "switches to the safeboot partition, uploads the image there (written to "
         "app0 / ota_0) and reboots into it.</p>";
    h += "<form action='/u2?fsz=' method='post' enctype='multipart/form-data' onsubmit=\"return upl(this.querySelector('button'))\">";
    h += "<input type='file' name='u2' id='f' accept='.bin'>";
    h += "<p><button type='submit' class='btn'>"
         "Start upgrade</button></p></form>";
    h += "<div id='f3' style='display:none' class='msg ok'>Switching to safeboot partition...</div>";
    h += "<div id='f2' style='display:none' class='msg ok'>Upload started...</div>";
    h += "<p class='muted'>If a bad image bricks the board, the bootloader falls back "
         "to the safeboot recovery automatically.</p>";
    h += "<p><a class='btn alt' href='/'>Back</a></p>";
     h += "<script>"
         "function eb(i){return document.getElementById(i);}"
         "var fctTries=0;"
         "function upl(t){var e=eb('f');"
         "if(!e.files||!e.files.length){alert('No file selected');return false;}"
         "var sl=e.files[0].slice(0,1);var rd=new FileReader();"
         "rd.onload=function(){var bb=new Uint8Array(rd.result);"
         "if(bb.length==1&&bb[0]==0xE9){fctTries=0;fct(t);}"
         "else{t.form.action='/u2?fsz='+e.files[0].size;t.form.submit();}};"
         "rd.readAsArrayBuffer(sl);return false;}"
         "function su(t){eb('f3').style.display='none';eb('f2').style.display='block';"
         "upRetry(t.form,0);}"
         "function upRetry(form,tries){"
         "var url='/u2?fsz='+eb('f').files[0].size;"
         "fetch(url,{method:'POST',body:new FormData(form)}).then(function(r){"
         "return r.text().then(function(s){return {st:r.status,body:s};});}).then(function(o){"
         "if(o.st===200&&o.body.indexOf('Upload successful')>=0){"
         "eb('f2').textContent='Upload done, rebooting into new firmware...';"
         "setTimeout(function(){pollApp(0);},8000);}"
         "else if(tries<3){eb('f2').textContent='Upload interrupted, retrying...';"
         "setTimeout(function(){upRetry(form,tries+1);},3000);}"
         "else{document.open();document.write(o.body);document.close();}}).catch(function(){"
         "if(tries<3){eb('f2').textContent='Upload interrupted, retrying...';"
         "setTimeout(function(){upRetry(form,tries+1);},3000);}"
         "else{eb('f2').textContent='Upload failed - device may still be reachable, retry manually.';}});}"
         "function pollApp(n){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.text();}).then(function(s){"
         "var ok=false;try{var j=JSON.parse(s);if(j&&j.wifi_mode)ok=true;}catch(e){}"
         "if(ok){location.href='/';}"
         "else if(n<60){setTimeout(function(){pollApp(n+1);},3000);}"
         "else{eb('f2').textContent='Device did not come back - it may have fallen back to safeboot. Open /up there.';}}).catch(function(){"
         "if(n<60){setTimeout(function(){pollApp(n+1);},3000);}"
         "else{eb('f2').textContent='Device did not come back - it may have fallen back to safeboot. Open /up there.';}});}"
         "function fct(t){var x=new XMLHttpRequest();"
         "x.open('GET','/u4?u4=fct&api=',true);"
         "x.onreadystatechange=function(){"
         "if(x.readyState==4&&x.status==200){var s=x.responseText;"
         "if(s==='false'){eb('f3').style.display='block';fctTries++;"
         "if(fctTries<60){setTimeout(function(){fct(t);},5000);}"
         "else{eb('f3').textContent='Safeboot is not answering. Join the L7-RECOVERY AP (172.218.28.1) or check STA, then open /up manually.';}}"
         "if(s==='true'){setTimeout(function(){su(t);},1000);}}"
         "else if(x.readyState==4&&x.status===0){fctTries++;"
         "if(fctTries<120){setTimeout(function(){fct(t);},2000);}}};"
         "x.send();}"
         "</script>";
    sendOtaPage(req, h);
    return ESP_OK;
}

// GET /u1 - URL upgrade not supported
static esp_err_t handleUpgradeUrl(httpd_req_t *req) {
    std::string h;
    h += "<h2>URL upgrade</h2>";
    h += "<p>URL-based OTA is not supported by this firmware. "
         "Use the manual file upload: <a class='btn' href='/up'>Firmware update</a>.</p>";
    h += "<p><a class='btn alt' href='/'>Back</a></p>";
    sendOtaPage(req, h);
    return ESP_OK;
}

// OPTIONS /u2 - CORS preflight
static esp_err_t handlePreflight(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "authorization, content-type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// GET /u4 - switch boot partition (fct | ota), Tasmota-compatible.
// Running from the app partition: u4=fct switches to safeboot right away.
static esp_err_t handleSwitchBoot(httpd_req_t *req) {
    char arg[8] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        char q[64];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            httpd_query_key_value(q, "u4", arg, sizeof(arg));
        }
    }
    bool api = false;
    {
        char q[128];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            // NOTE: val must be a real buffer - nullptr always yields
            // ESP_ERR_INVALID_ARG (httpd_parse.c), which broke api detection.
            char apiv[2] = {0};
            api = httpd_query_key_value(q, "api", apiv, sizeof(apiv)) == ESP_OK;
        }
    }

    if (strcmp(arg, "fct") == 0) {
        if (runningFromFactory()) {
            if (api) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "true"); return ESP_OK; }
            sendOtaPage(req, "<h2>Partition switch</h2><p>Already running the safeboot partition.</p>"
                             "<p><a class='btn alt' href='/'>Back</a></p>");
            return ESP_OK;
        }
        ESP_LOGI("WebServer", "[u4] switching to safeboot (erasing otadata)");
        prepRestartToSafeboot();
        if (api) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "false");
        } else {
            sendOtaPage(req, "<h2>Partition switch</h2><div class='msg ok'>Switching to safeboot "
                             "partition. Rebooting...</div>");
        }
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
        return ESP_OK;
    }

    if (strcmp(arg, "ota") == 0) {
        if (!runningFromFactory()) {
            if (api) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "true"); return ESP_OK; }
            sendOtaPage(req, "<h2>Partition switch</h2><p>Already running the OTA partition.</p>"
                             "<p><a class='btn alt' href='/'>Back</a></p>");
            return ESP_OK;
        }
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        if (!next) {
            if (api) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "none"); return ESP_OK; }
            sendOtaPage(req, "<h2>Partition switch</h2><div class='msg err'>No OTA partition.</div>"
                             "<p><a class='btn alt' href='/'>Back</a></p>");
            return ESP_OK;
        }
        esp_ota_set_boot_partition(next);
        esp_ota_mark_app_valid_cancel_rollback();
        if (api) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "false");
        } else {
            sendOtaPage(req, "<h2>Partition switch</h2><div class='msg ok'>Switching to the OTA "
                             "partition. Rebooting...</div>");
        }
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
        return ESP_OK;
    }

    if (api) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "none"); return ESP_OK; }
    sendOtaPage(req, "<h2>Partition switch</h2><p>Use <code>/u4?u4=fct</code> or "
                     "<code>/u4?u4=ota</code>.</p><p><a class='btn alt' href='/'>Back</a></p>");
    return ESP_OK;
}

// GET /dashboard - 1:1 web mirror of the 800x480 display for remote
// diagnosis. Geometry, fonts and colors replicate Dashboard::buildGrid/
// refresh exactly (see those for the canonical numbers).
static esp_err_t handleDashboardPage(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns=%27http://www.w3.org/2000/svg%27 viewBox=%270 0 32 32%27%3E%3Crect width=%2732%27 height=%2732%27 rx=%277%27 fill=%27%2312161c%27/%3E%3Crect x=%279%27 y=%276%27 width=%274%27 height=%2713%27 fill=%27%23ffbe5a%27/%3E%3Ccircle cx=%2711%27 cy=%2723%27 r=%275%27 fill=%27%23ffbe5a%27/%3E%3Cpolygon points=%2722,8 17,20 27,20%27 fill=%27%236ebeFF%27/%3E%3Ccircle cx=%2722%27 cy=%2720%27 r=%275%27 fill=%27%236ebeFF%27/%3E%3C/svg%3E\">"
        "<title>L7 Display - Dashboard mirror</title>"
              "<!-- fw:'" APP_FW_VERSION "' --><style>"
        "body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:#12161c;color:#e8e8e8;padding:16px}"
        "h1{font-size:16px;color:#9fc0ff;margin:4px 4px 12px}"
        ".sub{color:#7f8ba1;font-size:12px;margin:0 4px 12px}"
        "#scr{position:relative;width:800px;height:480px;background:#000;overflow:hidden}"
        ".cell{position:absolute;border:1px solid #3c3c3c;border-radius:6px;background:#121212;overflow:hidden}"
        ".cell div{position:absolute}"
        ".cell.stale{background:#c81e1e}"
        ".cell .nm{position:absolute;overflow:hidden;white-space:nowrap;text-align:center;color:#fff;"
        "font-family:'Ubuntu Mono',ui-monospace,Menlo,Consolas,monospace}"
        ".cell .lb{position:absolute;overflow:hidden;white-space:nowrap;"
        "font-family:'Ubuntu Mono',ui-monospace,Menlo,Consolas,monospace}"
        ".cell .t{color:#ffbe5a}.cell .h{color:#6ebeFF}.cell .b{color:#96dc8c}.cell .tm{color:#6e6e6e;text-align:right}"
        ".cell .r{color:#9aa0a6;text-align:right;white-space:nowrap;overflow:hidden}"
        ".cell svg{position:absolute}"
        "a{color:#9fc0ff}</style></head><body>"
        "<h1>L7 Display dashboard mirror (800x480)</h1>"
        "<p class='sub' id='hdr'>loading...</p>"
        "<div id='scr'></div>"
        "<p class='sub'><a href='/'>Setup UI</a></p>"
        "<script>"
        "var HRES=800,VRES=480,MARGIN=16,TOPBAR=34,GAP=12;"
        "function fonts(ch,cw){var v=Math.floor(ch/3);v=Math.max(24,Math.min(52,v));"
        "var n=Math.floor(ch/4);n=Math.max(16,Math.min(36,n));"
        "var s=Math.floor(ch/7);s=Math.max(12,Math.min(22,s));"
        "while(v>16&&(7*v+44)>cw)v-=2;"
        "return {v:v,n:n,s:s};}"
         "function cid(cc){return (typeof cc==='string')?cc:((cc&&typeof cc.id==='string')?cc.id:'');}"
         "function disc(x,y,d,col){return '<div style=\"position:absolute;left:'+x+'px;top:'+y+'px;width:'+d+'px;height:'+d+'px;border-radius:50%;background:'+col+';border:1px solid '+col+'\"></div>';}"
         "function bar(x,y,w,h,col,rad){return '<div style=\"position:absolute;left:'+x+'px;top:'+y+'px;width:'+w+'px;height:'+h+'px;background:'+col+';border-radius:'+rad+'px;border:1px solid '+col+'\"></div>';}"
         "function icoT(c){var D=c.ih/3;if(D>c.iw-2)D=c.iw-2;if(D<8)D=8;D=Math.floor(D);"
         "var sw=Math.max(3,Math.floor(c.iw/6));var sx=Math.floor((c.iw-sw)/2);"
         "var bulbY=c.ih-D,bulbX=Math.floor((c.iw-D)/2),bulbCy=bulbY+Math.floor(D/2);"
         "var h=disc(bulbX,bulbY,D,c.col);"
         "h+=bar(sx,2,sw,bulbCy-2+2,c.col,Math.floor(sw/2));"
         "for(var k=0;k<3;k++){var ty=8+k*Math.floor((bulbCy-12)/3);h+=bar(sx-5,ty,4,2,c.col,0);}"
         "return h;}"
         "function icoH(c){var D=c.ih/2;if(D>c.iw-2)D=c.iw-2;if(D<8)D=8;D=Math.floor(D);"
         "var discY=c.ih-D,cx=Math.floor(c.iw/2);"
         "var h=disc(Math.floor((c.iw-D)/2),discY,D,c.col);"
         "var tw=D-2;if(tw<6)tw=6;var th=discY+Math.floor(D/2);if(th<4)th=4;"
         "h+='<div style=\"position:absolute;left:'+(cx-Math.floor(tw/2))+'px;top:0px;width:0px;height:0px;"
         "border-left:'+Math.floor(tw/2)+'px solid transparent;border-right:'+Math.floor(tw/2)+'px solid transparent;"
         "border-bottom:'+th+'px solid '+c.col+'\"></div>';"
         "return h;}"
         "function icoB(c){var h='<div style=\"position:absolute;left:0px;top:0px;width:20px;height:18px;border:1px solid '+c.col+';border-radius:2px\"></div>';"
         "h+=bar(19,6,3,6,c.col,1);"
         "if(c.lvl>0){var fw=Math.floor(16*c.lvl/100);if(fw>0)h+=bar(2,2,fw,14,c.col,0);}"
         "return h;}"
         "function icoR(c){"
         "var h=bar(8,7,3,7,c.col,1);"
         "h+='<div style=\"position:absolute;left:0px;top:0px;width:18px;height:9px;overflow:hidden\">"
         "<div style=\"position:absolute;left:1px;top:4px;width:16px;height:16px;border:1px solid '+c.col+';border-radius:50%\"></div>"
         "<div style=\"position:absolute;left:4px;top:7px;width:10px;height:10px;border:1px solid '+c.col+';border-radius:50%\"></div>"
         "</div>';return h;}"
         "function tstr(age){if(age<90)return 'just now';if(age<3600)return Math.floor(age/60)+' min ago';return Math.floor(age/3600)+' h ago';}"
         "function esc(s){return String(s===undefined||s===null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}"
         "function wfan(r){var lv,col;"
         "if(r===null||r===undefined){lv=0;col='#787878';}"
         "else if(r>=-50){lv=3;col='#50dc78';}"
         "else if(r>=-65){lv=2;col='#50dc78';}"
         "else if(r>=-75){lv=1;col='#ebc85a';}"
         "else if(r>=-85){lv=1;col='#eb9646';}"
         "else{lv=0;col='#eb5a5a';}"
         "for(var i=0;i<3;i++){var e=document.getElementById('wfan'+i);"
         "if(e){e.style.display=i<lv?'block':'none';e.style.borderTopColor=col;}}"
         "var d=document.getElementById('wfdot');if(d)d.style.background=col;}"
         "async function getj(path){"
         "const ctl=(typeof AbortController!=='undefined')?new AbortController():null;"
         "let to=null;"
         "try{"
         "if(ctl)to=setTimeout(function(){ctl.abort();},10000);"
         "const r=await fetch(path,{cache:'no-store',signal:ctl?ctl.signal:undefined});"
         "const ct=r.headers.get('content-type')||'?';"
         "const t=await r.text();"
         "if(r.status!==200)throw path+' http '+r.status+' ct='+ct+' body='+t.slice(0,60);"
         "if(t.charAt(0)==='<')throw path+' HTML page ct='+ct+' body='+t.slice(0,60);"
         "try{return JSON.parse(t);}catch(e){throw path+' bad JSON ct='+ct+' body='+t.slice(0,60);}"
         "}finally{if(to)clearTimeout(to);}}"
         "async function ref(){"
         "const hdr=document.getElementById('hdr');"
         "function say(t){hdr.textContent=t;}"
         "try{"
         "say('fetching status...');"
         "const st=await getj('/api/status');"
         "say('fetching config...');"
         "const cfg=await getj('/api/config');"
         "const rows=cfg.grid_rows||2,cols=cfg.grid_cols||3;"
        "const cw=Math.floor((HRES-2*MARGIN-(cols-1)*GAP)/cols);"
        "const ch=Math.floor((VRES-TOPBAR-MARGIN-(rows-1)*GAP)/rows);"
         "const F=fonts(ch,cw);"
         "let j={sensors:[]};"
         "try{j=await getj('/api/sensors?all=1');}catch(e){say('sensors fetch failed, showing config: '+e);}"
         "const byId={};(j.sensors||[]).forEach(function(x){byId[x.id]=x;});"
        "const cells=cfg.cells||[];"
         "let anyPop=false;cells.forEach(function(cc){if(cid(cc))anyPop=true;});"
        "const scr=document.getElementById('scr');let h='';"
        "h+='<div style=\"position:absolute;left:0;top:0;width:800px;height:34px\"></div>';"
         "h+='<div style=\"position:absolute;right:16px;top:4px;width:36px;height:20px;overflow:hidden\">"
         "<div id=\"wfan0\" style=\"position:absolute;left:13px;top:11px;width:10px;height:10px;border:2px solid transparent;border-top-color:#888;border-radius:50%\"></div>"
         "<div id=\"wfan1\" style=\"position:absolute;left:8px;top:6px;width:20px;height:20px;border:2px solid transparent;border-top-color:#888;border-radius:50%\"></div>"
         "<div id=\"wfan2\" style=\"position:absolute;left:3px;top:1px;width:30px;height:30px;border:2px solid transparent;border-top-color:#888;border-radius:50%\"></div>"
         "</div>'+'<div id=\"wfdot\" style=\"position:absolute;left:763px;top:25px;width:6px;height:6px;border-radius:50%;background:#8f8\"></div>';"
        "if(!anyPop){"
        "let msg='No sensors configured\\n\\nPlease open the web UI and\\nconfigure Data Source + Dashboard';"
        "if(st.ip)msg+='\\n\\nConnect to:   '+st.ip;"
        "h+='<div style=\"position:absolute;left:16px;top:0;width:768px;height:480px;display:flex;align-items:center;justify-content:center;"
        "text-align:center;color:#fff;font-size:28px;font-family:monospace;white-space:pre-line\">'+msg.replace(/</g,'&lt;')+'</div>';"
        "scr.innerHTML=h;return;}"
         "let ncells=0,nlive=0;"
         "for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){"
         "const idx=r*cols+c;const id=cid(cells[idx]);"
         "if(!id)continue;"
         "ncells++;"
        "const x=MARGIN+c*(cw+GAP),y=TOPBAR+r*(ch+GAP);"
         "const s=byId[id];const have=!!s;if(have)nlive++;"
        "const nm=have?String(s.name ?? id):String(id);"
         "const tp=have?String(s.temp ?? ''):'',hu=have?String(s.hum ?? ''):'',bt=have?String(s.batt ?? ''):'',rs=have?String(s.rssi ?? ''):'';"
         "const hasT=have&&!!s.temp,hasH=have&&!!s.hum,hasB=have&&!!s.batt,hasR=have&&!!s.rssi;"
        "const age=have?(s.age_s||0):0;"
        "const tstr2=have?tstr(age):'';"
        "const stale=have&&age>3600;"
        "let yy=6;"
         "h+='<div class=\"cell'+(stale?' stale':'')+'\" style=\"left:'+x+'px;top:'+y+'px;width:'+cw+'px;height:'+ch+'px\">';"
          "h+='<div class=\"nm\" style=\"left:4px;top:'+yy+'px;width:'+(cw-8)+'px;font-size:'+F.n+'px;line-height:'+F.n+'px\">'+esc(nm)+'</div>';"
        "yy+=F.n+6;"
        "const contentH=ch-yy,rowH=F.v+8,battH=F.s+4;"
        "const nRows=(contentH-rowH-rowH-battH-14>=0)?3:2;"
        "let extra=Math.floor((contentH-nRows*rowH)/(nRows+1));if(extra<2)extra=2;"
        "yy+=extra;"
         "const iw=(F.v<36)?24:30,ih=F.v+8;"
          "const compact=(nRows===2),compactIcons=compact&&cw>=220;"
          "h+='<div style=\"left:6px;top:'+(yy-5)+'px\">'+(hasT?icoT({iw:iw,ih:ih,col:'#ffbe5a'}):'')+'</div>';"
          "h+='<div class=\"lb t\" style=\"left:'+(6+iw+4)+'px;top:'+(yy-2)+'px;font-size:'+F.v+'px'+(compact?';width:'+Math.max(20,(cw-(6+iw+4)-((compactIcons?20+4:0)+(compactIcons?80:58)+8)))+'px;overflow:hidden;white-space:nowrap;':'')+'\">'+tp+'</div>';"
          "if(compact&&hasB){if(compactIcons)h+='<div style=\"left:'+(cw-8-80-4-18)+'px;top:'+(yy-2)+'px\">'+icoB({col:'#96dc8c',lvl:parseInt(bt)||0})+'</div>';"
          "h+='<div style=\"left:'+(cw-8-80)+'px;top:'+(yy-2)+'px;width:80px;text-align:right;font-size:'+F.s+'px;color:#96dc8c\">'+bt+'</div>';}"
          "yy+=rowH+extra;"
          "h+='<div style=\"left:6px;top:'+(yy-7)+'px\">'+(hasH?icoH({iw:iw,ih:ih,col:'#6ebeFF'}):'')+'</div>';"
          "h+='<div class=\"lb h\" style=\"left:'+(6+iw+4)+'px;top:'+(yy-2)+'px;font-size:'+F.v+'px'+(compact?';width:'+Math.max(20,(cw-(6+iw+4)-((compactIcons?18+4:0)+80+8)))+'px;overflow:hidden;white-space:nowrap;':'')+'\">'+hu+'</div>';"
          "if(compact&&hasR){if(compactIcons)h+='<div style=\"left:'+(cw-8-80-4-18)+'px;top:'+(yy-2)+'px\">'+icoR({col:'#9aa0a6'})+'</div>';"
          "h+='<div style=\"left:'+(cw-8-80)+'px;top:'+(yy-2)+'px;width:80px;text-align:right;font-size:'+F.s+'px;color:#9aa0a6\">'+rs+'</div>';}"
         "const narrowBatt=(nRows===3&&cw<213),bFs=narrowBatt?18:F.s;"
          "if(nRows===3){yy+=rowH+extra;"
          "if(!narrowBatt)h+='<div style=\"left:6px;top:'+yy+'px\">'+(hasB?icoB({col:'#96dc8c',lvl:parseInt(bt)||0}):'')+'</div>';"
          "h+='<div class=\"lb b\" style=\"left:'+(narrowBatt?6:(6+22+4))+'px;top:'+(yy-2)+'px;font-size:'+bFs+'px'+(narrowBatt?';width:'+(cw-120)+'px;overflow:hidden;white-space:nowrap;':'')+'\">'+bt+'</div>';"
         "if(hasR){h+='<div style=\"left:'+(cw-104)+'px;top:'+yy+'px\">'+icoR({col:'#9aa0a6'})+'</div>';"
          "h+='<div class=\"lb r\" style=\"left:'+(cw-82)+'px;top:'+(yy-2)+'px;width:76px;font-size:'+bFs+'px\">'+rs+'</div>';}}"
         "h+='<div class=\"lb tm\" style=\"left:4px;top:'+(ch-F.s-6)+'px;width:'+(cw-8)+'px;font-size:'+F.s+'px\">'+tstr2+'</div>';"
         "h+='</div>';}"
         "scr.innerHTML=h;"
         "wfan(st.wifi_rssi);"
         "say('source: '+st.data_source);"
         "}catch(e){document.getElementById('hdr').textContent='load failed: '+e;}}"
        "ref();setInterval(ref,5000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /safeboot - erase otadata and reboot into the safeboot recovery firmware
static esp_err_t handleSafeboot(httpd_req_t *req) {
    sendOtaPage(req, "<h2>Safeboot (recovery)</h2>"
                     "<div class='msg ok'>Erasing otadata and rebooting into the safeboot "
                     "recovery firmware. Reconnect to its access point / IP...</div>");
    prepRestartToSafeboot();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

// POST /u2 + GET /u3 - upload result / info. The actual flash write is done by
// the SAFEBOOT firmware; a direct upload here is not processed.
static esp_err_t handleUploadDone(httpd_req_t *req) {
    std::string h;
    h += "<h2>Firmware update</h2>";
    h += "<div class='msg err'>The image must be written from the safeboot "
         "partition. Use <a class='btn' href='/up'>Firmware update</a>, which "
         "switches automatically.</div>";
    h += "<p><a class='btn' href='/up'>Firmware update</a> "
         "<a class='btn alt' href='/'>Back</a></p>";
    sendOtaPage(req, h);
    return ESP_OK;
}

// POST /api/ota/flash - WebUI recovery button: erase otadata + reboot into the
// safeboot recovery UI.
static esp_err_t handleOtaFlash(httpd_req_t *req) {
    if (!checkAuth(req)) { resp401(req); return ESP_OK; }
    prepRestartToSafeboot();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

    // Captive-portal captive redirect (iPhone / Android compatibility):
    //
    // When a phone joins an open AP it probes well-known URLS (iOS:
    // /generate_204, /gateway.lbl.gov, ... ; Android: /generate_204, /connectivitycheck.gstatic.com)
    // and treats 204 "OK" or a redirect-to-a-page as "this is a captive portal".
    // A 404 is read as "stale/broken web server, not a portal" - no popup, "no internet".
    //
    // Safeboot works because its error handler 302-redirects every unknown path
    // to "/" (the canonical captive trick). Mirror it here so the app AP also
    // triggers the iOS/Android captive popup and the /generate_204 probes land on
    // the setup page.
    static esp_err_t handlerNotFound(httpd_req_t *req, httpd_err_code_t err) {
        (void)err;
        // Relative redirect: keeps the browser on whichever address it used
        // (STA IP or hotspot IP). A hardcoded AP URL would bounce STA
        // clients to an unreachable address.
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

void WebServer::start() {
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 24;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        LogBuffer::logf("httpd start failed");
        return;
    }
    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.method = HTTP_GET;
    u.user_ctx = nullptr;

    u.uri = "/"; u.handler = handleRoot; httpd_register_uri_handler(s_server, &u);
    u.uri = "/dashboard"; u.handler = handleDashboardPage; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/status"; u.handler = handleApiStatus; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/display"; u.handler = handleDisplay; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/info"; u.handler = handleApiInfo; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/config"; u.handler = handleApiConfig; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/wifi/scan"; u.handler = handleWifiScan; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/sensors"; u.handler = handleListSensors; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/log"; u.handler = handleLog; httpd_register_uri_handler(s_server, &u);
    // Tasmota-compatible OTA endpoints
    u.uri = "/up"; u.handler = handleUpgradePage; httpd_register_uri_handler(s_server, &u);
    u.uri = "/u1"; u.handler = handleUpgradeUrl; httpd_register_uri_handler(s_server, &u);
    // GET /u2 serves the working upload form (a bare GET here used to fall
    // through to the stock 405 "Specified method is invalid" page).
    u.uri = "/u2"; u.handler = handleUpgradePage; httpd_register_uri_handler(s_server, &u);
    u.uri = "/u3"; u.handler = handleUploadDone; httpd_register_uri_handler(s_server, &u);
    u.uri = "/u4"; u.handler = handleSwitchBoot; httpd_register_uri_handler(s_server, &u);
    u.uri = "/safeboot"; u.handler = handleSafeboot; httpd_register_uri_handler(s_server, &u);

    u.method = HTTP_OPTIONS;
    u.uri = "/u2"; u.handler = handlePreflight; httpd_register_uri_handler(s_server, &u);

    u.method = HTTP_POST;
    // POST /u2 to the APP has no image consumer (flashing happens in
    // safeboot). Serve the working form immediately instead of hanging on
    // the unread multi-MB body until the browser times out.
    u.uri = "/u2"; u.handler = handleUpgradePage; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/save"; u.handler = handleApiSave; httpd_register_uri_handler(s_server, &u);
    u.method = HTTP_POST;
    u.uri = "/api/display"; u.handler = handleDisplay; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/discover"; u.handler = handleDiscover; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/reboot"; u.handler = handleReboot; httpd_register_uri_handler(s_server, &u);
    u.uri = "/api/ota/flash"; u.handler = handleOtaFlash; httpd_register_uri_handler(s_server, &u);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handlerNotFound);
    // NB: the captive DNS itself runs only in hotspot mode (see
    // WiFiManager::startHotspot); in STA/client mode there is no portal.
    LogBuffer::logf("WebServer started (%s)", WiFiManager::getMode() == WiFiManager::Mode::HOTSPOT
        ? "captive portal active" : "STA mode, no portal");
}

void WebServer::stop() {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
}

void WebServer::onConfigChanged() {
    AppConfig cfg;
    ConfigManager::load(cfg);
    if (DataSource::apply(cfg)) {
        // source type changed on disk while running (same OOM trap as the
        // save path): a clean reboot beats a half-torn-down radio stack
        LogBuffer::logf("Source switched externally, rebooting");
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
        return;
    }
    Dashboard::applyConfig(cfg);
}