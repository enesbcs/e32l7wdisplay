// Safeboot / recovery firmware - single-slot OTA (Tasmota-compatible).
//
// Architecture (port of the tasmota-ota project into this ESP-IDF app):
//   * The OTA slot is app0 (ota_0). There is no app1 - an app never rewrites
//     the partition it runs from. Firmware updates are performed by THIS
//     recovery firmware on the factory partition:
//       1. The running app switches to the factory slot by erasing otadata
//          (GET /u4?u4=fct) and reboots. The bootloader falls back to the
//          factory partition.
//       2. Here the browser uploads the .bin to app0 (POST /u2) - either raw
//          (the display WebUI) or as a classic multipart form (stock Tasmota
//          UI / template page). The image is written via esp_ota, the boot
//          partition is set to app0 and the device reboots into it.
//   * Failure to boot the new image -> next boot falls back here again.
//
// Endpoints (Tasmota-faithful):
//   /     GET  info page          /up GET upload form
//   /u1   GET  URL upgrade (unsupported) -> info
//   /u2   OPTIONS CORS preflight, POST firmware .bin -> app0
//   /u3   GET  upload result
//   /u4   GET  switch boot partition (u4=fct|ota), Tasmota compatible
//   /wi   GET/POST WiFi settings (saved in the shared "safeboot" NVS)
//   *     unknown path -> captive-portal redirect while AP is up
//
// WiFi: tries STA with saved credentials, AP (172.218.28.1) is ALWAYS up so
// the recovery UI stays reachable. Credentials come from the shared NVS
// namespace "safeboot" (written by the display app), then from the legacy
// sta_ssid/sta_pass keys, then from the ESP-IDF/Tasmota nvs.net80211 store.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"

#include "DnsServer.hpp"

#define TAG "safeboot"

#define NVS_NS            "safeboot"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "pass"
#define NVS_KEY_SSID_LEG  "sta_ssid"
#define NVS_KEY_PASS_LEG  "sta_pass"

// ESP-IDF / Tasmota WiFi driver persistent store
#define TASM_NVS_NS       "nvs.net80211"
#define TASM_KEY_SSID     "sta.ssid"
#define TASM_KEY_PASS     "sta.pswd"

#define WIFI_GOT_IP BIT0
#define WIFI_FAILED BIT1

#define AP_IPADDR         172, 218, 28, 1
#define AP_SSID           "L7-RECOVERY"

static EventGroupHandle_t s_wifi_events = nullptr;

// ---------------------------------------------------------------- utilities
static void connState(bool is_ap) {
    (void)is_ap;
}

static bool otadataMissing() {
    const esp_partition_t *od = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (!od) return true;
    uint8_t buf[32];
    if (esp_partition_read(od, 0, buf, sizeof(buf)) != ESP_OK) return true;
    for (int i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != 0xFF) return false;
    }
    return true;
}

static bool app0Bootable() {
    const esp_partition_t *app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!app || app->size == 0) return false;
    uint8_t magic = 0xFF;
    if (esp_partition_read(app, 0, &magic, 1) != ESP_OK) return false;
    return magic == ESP_IMAGE_HEADER_MAGIC;
}

// -------------------------------------------------------------------- nvs
static void nvsWriteStr(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (val && *val) nvs_set_str(h, key, val);
        else nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvsReadStr(const char *key, std::string &out) {
    out.clear();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len <= 1) {
        nvs_close(h);
        return false;
    }
    std::string s(len - 1, '\0');
    if (nvs_get_str(h, key, s.data(), &len) == ESP_OK) {
        s.resize(len - 1);
        if (!s.empty()) out.swap(s);
    }
    nvs_close(h);
    return !out.empty();
}

// read the ESP-IDF/Tasmota wifi driver credentials (nvs.net80211)
static bool nvs80211Read(std::string &ssid, std::string &pass) {
    ssid.clear();
    pass.clear();
    nvs_handle_t h;
    if (nvs_open(TASM_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    auto blobStr = [&](const char *key, std::string &out) -> bool {
        out.clear();
        size_t sz = 0;
        if (nvs_get_blob(h, key, nullptr, &sz) != ESP_OK || sz == 0) return false;
        uint8_t buf[128];
        size_t len = sizeof(buf);
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len == 0) return false;
        size_t n = 0;
        while (n < len && buf[n] != 0) n++;
        if (n == 0) return false;
        out.assign((const char *)buf, n);
        return true;
    };

    bool ok = blobStr(TASM_KEY_SSID, ssid);
    blobStr(TASM_KEY_PASS, pass);
    nvs_close(h);
    return ok && !ssid.empty();
}

static bool loadCreds(std::string &ssid, std::string &pass) {
    if (nvsReadStr(NVS_KEY_SSID, ssid)) {
        nvsReadStr(NVS_KEY_PASS, pass);
        return true;
    }
    if (nvsReadStr(NVS_KEY_SSID_LEG, ssid)) {
        nvsReadStr(NVS_KEY_PASS_LEG, pass);
        return true;
    }
    return nvs80211Read(ssid, pass);
}

// ------------------------------------------------------------------- wifi
static void wifiEventHandler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "STA disconnected");
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP);
    }
    (void)arg;
}

static void startWifi() {
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);

    std::string ssid, pass;
    bool hasCreds = loadCreds(ssid, pass);

    if (s_wifi_events == nullptr) s_wifi_events = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr);

    // AP is always up so the recovery UI stays reachable
    esp_wifi_set_mode(hasCreds ? WIFI_MODE_APSTA : WIFI_MODE_AP);

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_ip_info_t ip;
        ip.ip.addr = esp_ip4addr_aton("172.218.28.1");
        ip.gw.addr = esp_ip4addr_aton("172.218.28.1");
        ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        esp_netif_dhcps_stop(ap);
        esp_netif_set_ip_info(ap, &ip);
        esp_netif_dhcps_start(ap);
    }
    wifi_config_t apcfg = {};
    strlcpy((char *)apcfg.ap.ssid, AP_SSID, sizeof(apcfg.ap.ssid));
    apcfg.ap.ssid_len = strlen(AP_SSID);
    apcfg.ap.max_connection = 4;
    apcfg.ap.authmode = WIFI_AUTH_OPEN;
    apcfg.ap.channel = 6;
    esp_wifi_set_config(WIFI_IF_AP, &apcfg);

    if (hasCreds) {
        wifi_config_t stacfg = {};
        strlcpy((char *)stacfg.sta.ssid, ssid.c_str(), sizeof(stacfg.sta.ssid));
        strlcpy((char *)stacfg.sta.password, pass.c_str(), sizeof(stacfg.sta.password));
        stacfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        stacfg.sta.pmf_cfg.capable = true;
        stacfg.sta.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_STA, &stacfg);
        ESP_LOGI(TAG, "STA credentials present, connecting to '%s'", ssid.c_str());
    }

    esp_wifi_start();

    if (hasCreds) {
        xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP | WIFI_FAILED);
        esp_wifi_connect();
        xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP | WIFI_FAILED, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(12000));
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_GOT_IP)) {
            esp_wifi_disconnect();
        }
    }
    dns_captive_start();
    ESP_LOGI(TAG, "wifi up (AP always live at 172.218.28.1), recovery mode");
}

// HTML-escape for SSID reflection (rogue AP names reach these pages;
// safeboot has no auth by design, so output escaping is the barrier)
static std::string escHtml(const std::string &v) {
    std::string o;
    for (char c : v) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else if (c == '\'') o += "&#39;";
        else o += c;
    }
    return o;
}

static std::string wifiInfoLine() {
    std::string ssid, pass;
    if (loadCreds(ssid, pass)) {
        return "STA \"" + escHtml(ssid) + "\" (saved)";
    }
    return "AP only";
}

// --------------------------------------------------------------------- web
static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns=%27http://www.w3.org/2000/svg%27 viewBox=%270 0 32 32%27%3E%3Crect width=%2732%27 height=%2732%27 rx=%277%27 fill=%27%2312161c%27/%3E%3Crect x=%279%27 y=%276%27 width=%274%27 height=%2713%27 fill=%27%23ffbe5a%27/%3E%3Ccircle cx=%2711%27 cy=%2723%27 r=%275%27 fill=%27%23ffbe5a%27/%3E%3Cpolygon points=%2722,8 17,20 27,20%27 fill=%27%236ebeFF%27/%3E%3Ccircle cx=%2722%27 cy=%2720%27 r=%275%27 fill=%27%236ebeFF%27/%3E%3C/svg%3E\">"
    "<title>L7 Recovery</title><style>"
    "body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:#12161c;color:#e8e8e8;padding:16px}"
    ".card{background:#1b212b;border:1px solid #2a3038;border-radius:8px;padding:16px;max-width:660px;margin:12px auto}"
    "h1{font-size:20px;margin:0 0 2px}h2{font-size:15px;margin:0 0 10px;color:#9fc0ff}"
    "table{width:100%;border-collapse:collapse;font-size:13px}td{padding:4px 6px;border-bottom:1px solid #2e3138}td.k{color:#9fc0ff;width:55%}"
    ".btn{display:inline-block;background:#2f6fd0;color:#fff;border:0;border-radius:6px;padding:10px 16px;font-size:14px;font-weight:600;text-decoration:none;cursor:pointer;margin:2px}"
    ".btn.alt{background:#232b38}.btn.warn{background:#b3541e}"
    "input[type=file],input[type=text],input[type=password]{width:100%;padding:8px;border-radius:6px;border:1px solid #333;background:#12161c;color:#fff;margin:4px 0;box-sizing:border-box}"
    ".msg.ok{background:#0d2c15;color:#4ade80;padding:10px;border-radius:6px;margin:8px 0}"
    ".msg.err{background:#3a0d0d;color:#f87171;padding:10px;border-radius:6px;margin:8px 0}"
    ".muted{color:#7f8ba1;font-size:12px}"
    "</style></head><body>";

static const char HTML_FOOT[] = "</body></html>";

static void sendPage(httpd_req_t *req, const std::string &content) {
    httpd_resp_set_type(req, "text/html");
    std::string s(HTML_HEAD);
    s += "<div class='card'><h1>L7 Display - recovery</h1>";
    s += "<span class='muted'>(safeboot / factory partition)</span>";
    s += content;
    s += "</div>";
    s += HTML_FOOT;
    httpd_resp_send(req, s.c_str(), s.size());
    (void)connState;
}

// ------------------------------------------------------------------ GET /
static esp_err_t handlerRoot(httpd_req_t *req) {
    std::string h;
    h += "<table>";
    h += "<tr><td class='k'>Chip</td><td>ESP32-S3</td></tr>";
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        char b[64];
        snprintf(b, sizeof(b), "%s @ 0x%06X", run->label, (int)run->address);
        h += std::string("<tr><td class='k'>Running</td><td>") + b + "</td></tr>";
    }
    h += "<tr><td class='k'>Flash freq / mode</td><td>40 MHz / DIO</td></tr>";
    h += "<tr><td class='k'>WiFi</td><td>" + wifiInfoLine() + "</td></tr>";

    h += "<tr><td class='k'>app0 (ota_0)</td><td>" +
         std::string(app0Bootable() ? "bootable" : "empty / invalid") + "</td></tr>";
    h += "<tr><td class='k'>otadata</td><td>" +
         std::string(otadataMissing() ? "cleared" : "present") + "</td></tr>";
    h += "</table>";

    const esp_partition_t *app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (app) {
        char b[80];
        snprintf(b, sizeof(b),
                 "<tr><td class='k'>app0 size</td><td>%u KB</td></tr>",
                 (unsigned)(app->size / 1024));
        h += b;
    }

    h += "<p style='margin-top:14px'>"
         "<a class='btn' href='/up'>Firmware update</a> "
         "<a class='btn alt' href='/wi'>WiFi settings</a></p>";

    h += "<h2>Partition table (8 MB, single OTA slot)</h2><table>";
    h += "<tr><td class='k'>safeboot</td><td>0x10000 - 0xE0000 (recovery)</td></tr>";
    h += "<tr><td class='k'>app0</td><td>0xE0000 - 0x3B0000 (OTA slot)</td></tr>";
    h += "<tr><td class='k'>spiffs</td><td>0x3B0000 - 0x800000</td></tr>";
    h += "</table>";

    sendPage(req, h);
    return ESP_OK;
}

// ------------------------------------------------------------------ GET /up
static esp_err_t handlerUpgrade(httpd_req_t *req) {
    std::string h;
    h += "<h2>Firmware upload</h2>";
    h += "<p class='muted'>Upload an ESP32 .bin (the OTA image). It is written to "
         "<b>app0 (ota_0)</b> and the device reboots into it. A bad image falls "
         "back here automatically.</p>";
    h += "<form action='/u2?fsz=' method='post' enctype='multipart/form-data' onsubmit=\"return up(this.querySelector('button'))\">";
    h += "<input type='file' name='u2' id='f' accept='.bin'>";
    h += "<button type='submit' class='btn'>Upload and reboot</button>";
    h += "</form>";
    h += "<div id='m' class='muted' style='margin-top:6px'></div>";
    h += "<script>"
         "function up(t){var e=document.getElementById('f');"
         "if(!e.files||!e.files.length){alert('No file selected');return false;}"
         "var s=e.files[0].size;t.form.action='/u2?fsz='+s;"
         "document.getElementById('m').textContent='Uploading '+s+' bytes...';"
         "return true;}</script>";
    h += "<p><a class='btn alt' href='/'>Back</a></p>";
    sendPage(req, h);
    return ESP_OK;
}

// ------------------------------------------------------------------ GET /u1
static esp_err_t handlerUpgradeUrl(httpd_req_t *req) {
    sendPage(req,
        "<h2>URL upgrade</h2>"
        "<p>URL-based OTA is not supported by this recovery firmware.</p>"
        "<p><a class='btn' href='/up'>Firmware update</a> "
        "<a class='btn alt' href='/'>Back</a></p>");
    return ESP_OK;
}

// ------------------------------------------------------- OPTIONS /u2
static esp_err_t handlerPreflight(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "authorization, content-type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// ------------------------------------------------------- POST /u2 - upload
// Accepts BOTH a raw octet-stream .bin (display WebUI fetch) and a classic
// multipart/form-data form. Multipart bodies are parsed minimally: the file
// part payload is extracted and fed to esp_ota_write.
struct UploadCtx {
    const esp_partition_t *app;
    esp_ota_handle_t oh;
    size_t total;
    size_t ok;              // bytes actually written to flash
    int    error;           // 0 ok, else reason code
};

static int multipartExtract(const uint8_t *body, size_t len, const uint8_t **data, size_t *dlen) {
    // body: --boundary\r\nheaders\r\n\r\nFILE...\r\n--boundary
    const char *b = (const char *)body;
    // boundary = first line (starts with "--")
    const char *nl = (const char *)memchr(b, '\n', len < 512 ? len : 512);
    if (!nl) return 5;
    size_t bndLen = (size_t)(nl - b);
    if (bndLen < 2) return 5;
    while (bndLen && (b[bndLen - 1] == '\r' || b[bndLen - 1] == '\n')) bndLen--;
    if (bndLen < 2 || b[0] != '-' || b[1] != '-') return 5;
    // find header/body separator: first CRLFCRLF after the boundary line
    const char *p = (const char *)memmem(nl, len - (size_t)(nl - b), "\r\n\r\n", 4);
    if (!p) return 5;
    p += 4;
    size_t dataOff = (size_t)(p - b);
    // payload ends at the closing "\r\n--<boundary>"
    const char *endMark = (const char *)memmem(p, len - dataOff, "\r\n--", 4);
    if (!endMark) return 5;
    *data = (const uint8_t *)p;
    *dlen = (size_t)(endMark - p);
    return 0;
}

static esp_err_t handlerOtaUpload(httpd_req_t *req) {
    const esp_partition_t *app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!app) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_OK;
    }

    size_t total = req->content_len;
    if (total < 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too small");
        return ESP_OK;
    }
    if (total > app->size + 128 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_OK;
    }

    // buffer the whole body (firmware can be ~2.9 MB; use PSRAM if present)
    uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = (uint8_t *)malloc(total);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    size_t got = 0;
    int r;
    while (got < total && (r = httpd_req_recv(req, (char *)(body + got), total - got)) > 0) {
        got += (size_t)r;
    }
    if (got != total) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error");
        return ESP_OK;
    }

    const uint8_t *data = body;
    size_t dlen = total;

    if (total >= 512 && body[0] == '-' && body[1] == '-') {
        // multipart form upload
        if (multipartExtract(body, total, &data, &dlen) != 0 || dlen < 1024) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad multipart");
            return ESP_OK;
        }
    } else if (body[0] == 0x1F) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gzip not supported");
        return ESP_OK;
    }

    if (data[0] != ESP_IMAGE_HEADER_MAGIC) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not an app image");
        return ESP_OK;
    }
    if (dlen > app->size) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "larger than app0");
        return ESP_OK;
    }

    esp_ota_handle_t oh = 0;
    if (esp_ota_begin(app, OTA_SIZE_UNKNOWN, &oh) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }

    bool bad = false;
    size_t written = 0;
    const size_t CHUNK = 4096;
    while (written < dlen) {
        size_t n = dlen - written;
        if (n > CHUNK) n = CHUNK;
        if (esp_ota_write(oh, data + written, n) != ESP_OK) { bad = true; break; }
        written += n;
    }
    free(body);

    if (bad || written != dlen) {
        esp_ota_abort(oh);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write error");
        return ESP_OK;
    }
    if (esp_ota_end(oh) != ESP_OK) {
        esp_ota_abort(oh);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image verify failed");
        return ESP_OK;
    }

    esp_ota_set_boot_partition(app);
    // no rollback in this build, but harmless to confirm the entry
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "firmware %.3f MB written to app0, setting boot + reboot",
             dlen / (1024.0 * 1024.0));

    char buf[256];
    int tried = snprintf(buf, sizeof(buf),
             "<h2>Upload successful</h2><div class='msg ok'>Firmware written to app0 "
             "(%.3f MB). Rebooting...</div><script>setTimeout(function(){location='/';},2500);</script>",
             dlen / (1024.0 * 1024.0));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, buf);
    vTaskDelay(pdMS_TO_TICKS(1600));
    esp_restart();
    (void)req;
    return ESP_OK;
}

// ------------------------------------------------------------------ GET /u3
static esp_err_t handlerUploadDone(httpd_req_t *req) {
    sendPage(req,
        "<h2>Upload</h2><p class='muted'>Firmware uploads are handled by POST /u2. "
        "Open the <a href='/up'>update form</a>.</p>");
    return ESP_OK;
}

// ------------------------------------------------------------------ GET /u4
static esp_err_t handlerSwitch(httpd_req_t *req) {
    char arg[8] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        char q[64];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            httpd_query_key_value(q, "u4", arg, sizeof(arg));
        }
    }
    bool api = httpd_req_get_url_query_len(req) > 0;
    if (api) {
        char q[128];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            // NOTE: val must be a real buffer - nullptr always yields
            // ESP_ERR_INVALID_ARG (httpd_parse.c), which broke api detection.
            char apiv[2] = {0};
            api = httpd_query_key_value(q, "api", apiv, sizeof(apiv)) == ESP_OK;
        }
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    bool running_factory = (run && run->type == ESP_PARTITION_TYPE_APP &&
                            run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY);

    if (strcmp(arg, "fct") == 0) {
        // we run from factory/safeboot: nothing to do, answer "true"
        if (api) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "true");
            return ESP_OK;
        }
        sendPage(req, "<h2>Partition switch</h2>"
                      "<p>Already running the safeboot (factory) partition.</p>"
                      "<p><a class='btn alt' href='/'>Back</a></p>");
        return ESP_OK;
    }

    if (strcmp(arg, "ota") == 0 && running_factory) {
        const esp_partition_t *app = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
        if (!app || !app0Bootable()) {
            if (api) {
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "none");
                return ESP_OK;
            }
            sendPage(req, "<h2>Partition switch</h2><div class='msg err'>app0 is empty "
                          "or not bootable.</div><p><a class='btn alt' href='/'>Back</a></p>");
            return ESP_OK;
        }
        esp_ota_set_boot_partition(app);
        esp_ota_mark_app_valid_cancel_rollback();
        if (api) {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "true");
        } else {
            sendPage(req, "<h2>Partition switch</h2><div class='msg ok'>Switching to app0 "
                          "(ota_0). Rebooting...</div>");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    if (api) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "none");
        return ESP_OK;
    }
    sendPage(req, "<h2>Partition switch</h2><p>Use <code>/u4?u4=fct</code> or "
                  "<code>/u4?u4=ota</code>.</p><p><a class='btn alt' href='/'>Back</a></p>");
    return ESP_OK;
}

// ------------------------------------------------------------------ /wi
static void wifiRebootPage(httpd_req_t *req, const std::string &msg) {
    std::string h;
    h += "<h2>WiFi settings</h2><div class='msg ok'>" + msg + "</div>";
    h += "<script>setTimeout(function(){location='/';},2500);</script>";
    sendPage(req, h);
}

static esp_err_t handlerWifi(httpd_req_t *req) {
    if (req->method == HTTP_POST) {
        char body[512] = {0};
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len < 0) len = 0;
        body[len] = '\0';

        std::string b(body);
        auto field = [&](const char *key) -> std::string {
            std::string pref = std::string(key) + "=";
            size_t pos = b.find(pref);
            if (pos == std::string::npos) return "";
            size_t start = pos + pref.size();
            size_t end = b.find('&', start);
            if (end == std::string::npos) end = b.size();
            std::string v = b.substr(start, end - start);
            // minimal url-decode
            std::string out;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '+') out += ' ';
                else if (v[i] == '%' && i + 2 < v.size()) {
                    char t[3] = {v[i+1], v[i+2], 0};
                    out += (char)strtol(t, nullptr, 16);
                    i += 2;
                } else out += v[i];
            }
            return out;
        };

        std::string action = field("action");
        if (action == "reset") {
            nvsWriteStr(NVS_KEY_SSID, nullptr);
            nvsWriteStr(NVS_KEY_PASS, nullptr);
            nvsWriteStr(NVS_KEY_SSID_LEG, nullptr);
            nvsWriteStr(NVS_KEY_PASS_LEG, nullptr);
            wifiRebootPage(req, "WiFi settings cleared. Rebooting to AP mode...");
            vTaskDelay(pdMS_TO_TICKS(600));
            esp_restart();
            return ESP_OK;
        }

        std::string ssid = field("ssid");
        std::string pass = field("pass");
        if (ssid.empty()) {
            sendPage(req, "<h2>WiFi settings</h2><div class='msg err'>SSID is required.</div>"
                          "<p><a class='btn alt' href='/wi'>Back</a></p>");
            return ESP_OK;
        }
        if (ssid.size() >= 32) ssid.resize(31);
        if (pass.size() >= 64) pass.resize(63);

        nvsWriteStr(NVS_KEY_SSID, ssid.c_str());
        nvsWriteStr(NVS_KEY_PASS, pass.c_str());
        nvsWriteStr(NVS_KEY_SSID_LEG, ssid.c_str());
        nvsWriteStr(NVS_KEY_PASS_LEG, pass.c_str());

        wifiRebootPage(req, "Saved. Connecting to <b>" + escHtml(ssid) + "</b>. Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
        return ESP_OK;
    }

    std::string cur;
    std::string ssid, pass;
    if (loadCreds(ssid, pass)) cur = ssid;
    std::string h;
    h += "<h2>WiFi settings</h2>";
    h += "<p class='muted'>Credentials are stored in a shared NVS namespace, "
         "so the display app can also use them.</p>";
    h += "<form method='post' action='/wi'>";
    h += "<label>SSID</label><input type='text' name='ssid' maxlength='32' value='" + escHtml(cur) + "' required>";
    h += "<label>Password</label><input type='password' name='pass' maxlength='64'>";
    h += "<button class='btn' type='submit'>Save and reboot</button></form>";
    h += "<form method='post' action='/wi'><input type='hidden' name='action' value='reset'>"
         "<button class='btn warn' type='submit'>Reset to AP mode</button></form>";
    h += "<p><a class='btn alt' href='/'>Back</a></p>";
    sendPage(req, h);
    return ESP_OK;
}

// ------------------------------------------------------------ captive 404
static esp_err_t handlerNotFound(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    // Relative redirect: keeps the browser on whichever address it used
    // (STA IP or AP IP). A hardcoded AP URL would bounce STA clients to an
    // unreachable address.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t handlerUploadFormRedirect(httpd_req_t *req) {
    // A stale /u2 URL (refresh after an interrupted upload, manual entry)
    // has no upload context - send the user to the upload form to restart.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/up");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static void recoveryServer() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 16;   // 9 routes registered; default 8 drops POST /wi
    cfg.recv_wait_timeout = 15;  // multi-MB uploads over marginal WiFi
    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) return;

    httpd_uri_t u = {0};
    u.method = HTTP_GET;

    u.uri = "/";   u.handler = handlerRoot;       httpd_register_uri_handler(srv, &u);
    u.uri = "/up"; u.handler = handlerUpgrade;    httpd_register_uri_handler(srv, &u);
    u.uri = "/u2"; u.handler = handlerUploadFormRedirect; httpd_register_uri_handler(srv, &u);
    u.uri = "/u1"; u.handler = handlerUpgradeUrl; httpd_register_uri_handler(srv, &u);
    u.uri = "/u3"; u.handler = handlerUploadDone; httpd_register_uri_handler(srv, &u);
    u.uri = "/u4"; u.handler = handlerSwitch;      httpd_register_uri_handler(srv, &u);

    u.method = HTTP_OPTIONS;
    u.uri = "/u2"; u.handler = handlerPreflight;  httpd_register_uri_handler(srv, &u);

    u.method = HTTP_POST;
    u.uri = "/u2"; u.handler = handlerOtaUpload;  httpd_register_uri_handler(srv, &u);

    struct { const char *uri; httpd_method_t method; esp_err_t (*h)(httpd_req_t *); } any[] = {
        { "/wi", (httpd_method_t)HTTP_GET,  handlerWifi },
        { "/wi", (httpd_method_t)HTTP_POST, handlerWifi },
    };
    for (auto &a : any) { u.uri = a.uri; u.method = a.method; u.handler = a.h; httpd_register_uri_handler(srv, &u); }

    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, handlerNotFound);

    ESP_LOGI(TAG, "recovery web: AP 172.218.28.1, / = info, /up = firmware");
}

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "=== L7 safeboot recovery (single-slot OTA) ===");

    startWifi();
    recoveryServer();

    while (true) { vTaskDelay(pdMS_TO_TICKS(60000)); }
}