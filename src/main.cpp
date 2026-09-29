#include <cstdio>
#include <cstdlib>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"

#include "esp_display_panel.hpp"
#include "esp_lvgl_port.h"
#include <lvgl.h>

#include "PinConfig.hpp"
#include "CH422G.hpp"
#include "AppConfig.hpp"
#include "WiFiManager.hpp"
#include "HAWebSocket.hpp"
#include "Integration.hpp"
#include "TasmotaWiFi.hpp"
#include "Dashboard.hpp"
#include "WebServer.hpp"
#include "DataSource.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"
#include "Version.hpp"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static AppConfig s_appConfig;
static bool s_wifiOk = false;
static bool s_apMode = false;
// staged boot: STA-up anchor + phase (0 = armed, 1 = source done, 2 = done).
// The delays are boot-storm mitigation, not functional waits: the blocking
// WiFi connect (with DHCP) is already done when the anchor is set, but the
// BLE-controller init (source) and the MQTT task+timers (integration) must
// not collide with each other or with the STA-up transients (that collision
// was the esp_timer ISR boot-loop). Keep the ORDER and the SEPARATION;
// the absolute numbers may shrink, the gaps may not vanish.
// Timings: source at +1s, integration +2s after the source apply returns
// (previously fixed +3s/+8s wall clock).
static uint32_t s_stageBaseMs = 0;
static uint32_t s_stage1Ms = 0;
static int s_stage = 0;
static constexpr uint32_t STAGE_SOURCE_MS = 1000;
static constexpr uint32_t STAGE_INTEGRATION_GAP_MS = 2000;

void updateLiveConfig(const AppConfig &cfg) {
    s_appConfig = cfg;
}

AppConfig getLiveConfig() {
    return s_appConfig;
}

// mirror the effective WiFi credentials into the safeboot stub's NVS namespace
// so the recovery web UI can bring up STA on the same network. Keys match the
// Tasmota-OTA reference ("ssid"/"pass"); the legacy "sta_ssid"/"sta_pass" keys
// are kept in sync for backwards compatibility.
static void mirrorSafebootWifi(const std::string &ssid, const std::string &pass) {
    nvs_handle_t h;
    if (nvs_open("safeboot", NVS_READWRITE, &h) != ESP_OK) return;
    if (!ssid.empty()) {
        nvs_set_str(h, "ssid", ssid.c_str());
        nvs_set_str(h, "pass", pass.c_str());
        nvs_set_str(h, "sta_ssid", ssid.c_str());
        nvs_set_str(h, "sta_pass", pass.c_str());
    } else {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_erase_key(h, "sta_ssid");
        nvs_erase_key(h, "sta_pass");
    }
    nvs_commit(h);
    nvs_close(h);
}

// read creds saved by the safeboot /wi page (ssid/pass, legacy sta_* keys)
static bool readSafebootWifi(std::string &ssid, std::string &pass) {
    nvs_handle_t h;
    if (nvs_open("safeboot", NVS_READONLY, &h) != ESP_OK) return false;
    auto get = [&](const char *key, std::string &out) {
        out.clear();
        size_t len = 0;
        if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len <= 1) return;
        out.resize(len - 1);
        nvs_get_str(h, key, out.data(), &len);
    };
    get("ssid", ssid);
    if (ssid.empty()) get("sta_ssid", ssid);
    get("pass", pass);
    if (pass.empty()) get("sta_pass", pass);
    nvs_close(h);
    return !ssid.empty();
}

static void onConfigSaved() {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void applyTimezone(const std::string &tz) {
    // process-wide POSIX TZ (WebUI Settings, NTP server section);
    // empty falls back to the default (UTC would show the wrong wall time)
    const char *t = tz.empty() ? AppConfig::DEFAULT_TIMEZONE : tz.c_str();
    setenv("TZ", t, 1);
    tzset();
}

static void ntpInit(const std::string &server, const std::string &tz) {
    if (server.empty()) return;
    applyTimezone(tz);
    // simple SNTP init (do not reset on conflict)
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server.c_str());
    esp_sntp_init();
    LogBuffer::logf("NTP started: %s", server.c_str());
}

static TaskHandle_t s_wifiTask = nullptr;

static void wifiReconnectTask(void *arg) {
    // backoff on consecutive failures (30s doubling, 5min cap); a stable
    // link (or AP mode) means effectively stopped: wake, check, sleep
    uint32_t backoffMs = 30000;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(backoffMs));
        if (s_apMode) { backoffMs = 300000; continue; }
        if (WiFiManager::getMode() == WiFiManager::Mode::CLIENT) { backoffMs = 300000; continue; }
        LogBuffer::logf("WiFi reconnect attempt...");
        if (WiFiManager::connectWithConfig(s_appConfig)) {
            backoffMs = 30000;
            DataSource::apply(s_appConfig);
            Dashboard::applyConfig(s_appConfig);
            WebServer::onConfigChanged();
            LogBuffer::logf("WiFi reconnected");
            // integration trailed by 2s (and was missing here entirely:
            // without this it stayed silent until reboot/save). The gap (not
            // the absolute time) keeps the MQTT burst off the source burst.
            vTaskDelay(pdMS_TO_TICKS(2000));
            Integration::apply(s_appConfig);
        } else {
            backoffMs = backoffMs >= 300000 ? 300000 : backoffMs * 2;
            LogBuffer::logf("WiFi retry in %us", (unsigned)(backoffMs / 1000));
        }
    }
}

extern "C" void app_main() {
    LogBuffer::init();
    LogBuffer::logf("=== L7Display %s ===", __DATE__);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // read Tasmota WiFi creds (read only, never write)
    std::string tsSsid1, tsPwd1, tsSsid2, tsPwd2;
    bool hasTasmotaCreds = TasmotaWiFi::readCredentials(tsSsid1, tsPwd1, tsSsid2, tsPwd2);
    if (hasTasmotaCreds)
        LogBuffer::logf("Tasmota WiFi blob read: ssid1='%s'", tsSsid1.c_str());

    LogBuffer::logf("Init board + display...");
    Board *board = new Board();
    board->init();

    auto lcd = board->getLCD();
    if (!lcd) { LogBuffer::logf("FATAL: getLCD null"); return; }
    lcd->configFrameBufferNumber(2);
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
    bool ok = board->begin();
    LogBuffer::logf("board->begin %d", ok);

    auto io_expander = board->getIO_Expander();
    if (io_expander) {
        auto *exp = static_cast<esp_expander::CH422G*>(io_expander->getBase());
        if (exp) CH422G::setExpander(exp);
    }
    CH422G::begin();

    LogBuffer::logf("LVGL init...");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = NULL,
        .panel_handle = lcd->getRefreshPanelHandle(),
        .control_handle = NULL,
        .buffer_size = (uint32_t)(lcd->getFrameWidth() * 20),
        .double_buffer = false,
        .trans_size = (uint32_t)(lcd->getFrameWidth() * 20),
        .hres = (uint32_t)lcd->getFrameWidth(),
        .vres = (uint32_t)lcd->getFrameHeight(),
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .rounder_cb = NULL,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = true, .avoid_tearing = false },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    assert(disp != NULL);

    Dashboard::begin();

    LogBuffer::logf("Init LittleFS + config...");
    ConfigManager::initFS();
    SensorRegistry::init();
    Dashboard::initMcuTemp();

    ConfigManager::load(s_appConfig);

    // WiFi setup
    WiFiManager::begin();
    WiFiManager::setConfigSavedCallback(onConfigSaved);

    // Build effective SSID: own config preferred, fallback to Tasmota blob
    std::string effSsid = s_appConfig.wifi_ssid;
    std::string effPass = s_appConfig.wifi_password;
    std::string effSsid2 = s_appConfig.wifi_ssid2;
    std::string effPass2 = s_appConfig.wifi_password2;

    if (!s_appConfig.wifi_ssid.empty() || !s_appConfig.wifi_ssid2.empty()) {
        LogBuffer::logf("Using display config WiFi");
    } else {
        std::string sbSsid, sbPass;
        if (readSafebootWifi(sbSsid, sbPass) && !sbSsid.empty()) {
            effSsid = sbSsid;
            effPass = sbPass;
            LogBuffer::logf("Falling back to safeboot NVS WiFi: %s", effSsid.c_str());
        } else if (hasTasmotaCreds && !tsSsid1.empty()) {
            effSsid = tsSsid1;
            effPass = tsPwd1;
            effSsid2 = tsSsid2;
            effPass2 = tsPwd2;
            LogBuffer::logf("Falling back to Tasmota WiFi: %s", effSsid.c_str());
        }
    }

    bool useConfigCreds = !s_appConfig.wifi_ssid.empty() || !s_appConfig.wifi_ssid2.empty();
    AppConfig effWifi = s_appConfig;
    effWifi.wifi_ssid = useConfigCreds ? s_appConfig.wifi_ssid : effSsid;
    effWifi.wifi_password = useConfigCreds ? s_appConfig.wifi_password : effPass;
    effWifi.wifi_ssid2 = useConfigCreds ? s_appConfig.wifi_ssid2 : effSsid2;
    effWifi.wifi_password2 = useConfigCreds ? s_appConfig.wifi_password2 : effPass2;

    if (effWifi.wifi_ssid.empty() && effWifi.wifi_ssid2.empty()) {
        LogBuffer::logf("No WiFi configured, starting hotspot...");
        mirrorSafebootWifi("", "");
        WiFiManager::startHotspot();
        s_apMode = true;
        Dashboard::showSetupScreen(WiFiManager::getAPName(), "172.218.28.1");
    } else {
        mirrorSafebootWifi(!effWifi.wifi_ssid.empty() ? effWifi.wifi_ssid : effWifi.wifi_ssid2,
                           !effWifi.wifi_ssid.empty() ? effWifi.wifi_password : effWifi.wifi_password2);
        if (WiFiManager::connectWithConfig(effWifi)) {
            s_wifiOk = true;
            LogBuffer::logf("WiFi connected: %s", WiFiManager::getIP().c_str());
        } else {
            LogBuffer::logf("WiFi failed");
            if (s_appConfig.hotspot_on_fail || !useConfigCreds) {
                WiFiManager::startHotspot();
                s_apMode = true;
                Dashboard::showSetupScreen(WiFiManager::getAPName(), "172.218.28.1");
            } else {
                // 8K stack: reconnect runs connect + full source apply.
                // Create-once guard: a second task would fight over the
                // radio with duplicate applies and LVGL rebuilds.
                if (!s_wifiTask)
                    xTaskCreate(wifiReconnectTask, "wifi_retry", 8192, nullptr, 3, &s_wifiTask);
                Dashboard::showNeedsConfigScreen();
            }
        }
    }

    WebServer::start();

    // heavy starts are staged from the 1Hz loop below (source at +1s,
    // integration +2s after the source apply) so BLE-controller/MQTT
    // heap+timer bursts do not collide seconds after STA-up (boot-storm
    // crash mitigation)
    if (!s_apMode) {
        s_stageBaseMs = (uint32_t)(esp_timer_get_time() / 1000);
        s_stage = 0;
    }
    ntpInit(s_appConfig.ntp_server, s_appConfig.timezone);

    LogBuffer::logfSerial("System ready (fw v%s)", APP_FW_VERSION);

    // --- task watchdog: reboot if the main loop stalls ---
    // NOTE: IDF already starts TWDT at boot, so init() returns
    // ESP_ERR_INVALID_STATE here — that is fine, we just reconfigure
    // the timeout and subscribe the main task.
    {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms = 30000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        esp_err_t r = esp_task_wdt_init(&wdt_cfg);
        if (r == ESP_ERR_INVALID_STATE) {
            esp_task_wdt_reconfigure(&wdt_cfg);
            r = ESP_OK;
        }
        if (r == ESP_OK) {
            esp_err_t a = esp_task_wdt_add(NULL);
            if (a == ESP_OK || a == ESP_ERR_INVALID_STATE) {
                LogBuffer::logfSerial("WDT armed: 30s");
            } else {
                LogBuffer::logf("WDT subscribe failed: %s", esp_err_to_name(a));
            }
        } else {
            LogBuffer::logf("WDT init failed: %s", esp_err_to_name(r));
        }
    }

    // --- heartbeat loop ---
    uint32_t lastRefresh = 0;
    // Heap gauge: logged to the same LogBuffer every 5s so the serial monitor
    // shows heap_free:NNN -> separates "low heap / leak" from the noise-level
    // "wifi:" driver warning we saw once a second (cosmetic, not fatal).
    uint32_t lastHeapLog = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_task_wdt_reset();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        // heap gauge: 30s cadence is enough (5s flooded the RAM log ring
        // and evicted real diagnostics within a minute).
        // NOTE: no integrity walk here - it traverses every block of the
        // multi-MB PSRAM heap and its OPI bursts visibly disturbed the RGB
        // scanout (single-frame jump). Counters below only walk free lists.
        // Corruption is checked once at boot and after heavy churn
        // (applyConfig rebuild, HA states bulk).
        if (now - lastHeapLog >= 30000) {
            lastHeapLog = now;
            static bool bootChecked = false;
            if (!bootChecked && now > 60000) {
                bootChecked = true;
                if (!heap_caps_check_integrity_all(true))
                    LogBuffer::logfSerial("HEAP INTEGRITY FAIL (boot check)");
                else
                    LogBuffer::logfSerial("HEAP INTEGRITY ok (boot check)");
            }
            // total includes PSRAM: internal/DMA separately, the BT controller
            // and WiFi mgmt frames need INTERNAL ram (that is what "m f null"
            // runs out of while total heap looks fine).
            LogBuffer::logfSerial("heap_free:%u internal:%u dma:%u largest:%u",
                (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }

        if (now - lastRefresh >= 1000) {
            lastRefresh = now;
            Dashboard::refresh(now);
            HAWebSocket::tick(now);
            Integration::tick(now);
            if (!s_apMode && s_stageBaseMs != 0 && s_stage < 2) {
                if (s_stage == 0 && now - s_stageBaseMs >= STAGE_SOURCE_MS) {
                    s_stage = 1;
                    DataSource::apply(s_appConfig);
                    Dashboard::applyConfig(s_appConfig);
                    s_stage1Ms = (uint32_t)(esp_timer_get_time() / 1000);
                } else if (s_stage == 1 && s_stage1Ms != 0
                           && now - s_stage1Ms >= STAGE_INTEGRATION_GAP_MS) {
                    s_stage = 2;
                    Integration::apply(s_appConfig);
                }
            }
        }
    }
}