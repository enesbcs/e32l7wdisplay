#pragma once

#include <string>
#include <vector>
#include <map>
#include "lvgl.h"
#include "AppConfig.hpp"

// Grid dashboard UI (LVGL). Built from AppConfig.grid_rows/cols + cells[].
enum class DashState {
    NONE,
    SETUP,        // access point mode -> show IP + SSID
    NEEDS_CONFIG, // wifi ok but no sensor configured
    DASHBOARD,    // sensor grid
};

class Dashboard {
public:
    static void begin();                 // create root widgets + fonts
    // (re)build grid; false = display busy/missing (caller reports 503)
    static bool applyConfig(const AppConfig &config);
    static void showSetupScreen(const std::string &ssid, const std::string &ip);
    static void showNeedsConfigScreen();
    static void refresh(uint32_t nowMs); // called ~1s from main loop

    // blank/unblank the backlight (avoids LCD flicker while OTA is flashing)
    static void blankDisplay(bool off);
    static bool isDisplayOn(); // backlight state (single source of truth)
    // on-chip temperature sensor (Info tab + integration); install once
    static void initMcuTemp();
    static bool readMcuTempC(float &out);

    static DashState state() { return s_state; }

private:
    struct Cell {
        std::string sensorId;
        lv_obj_t *container = nullptr;
        lv_obj_t *nameLabel = nullptr;
        lv_obj_t *tempLabel = nullptr;
        lv_obj_t *humLabel = nullptr;
        lv_obj_t *battLabel = nullptr;
        lv_obj_t *battFill = nullptr;
        lv_obj_t *tempIconRoot = nullptr;
        lv_obj_t *humIconRoot = nullptr;
        lv_obj_t *battIconRoot = nullptr;
        lv_obj_t *rssiIconRoot = nullptr;
        lv_obj_t *rssiLabel = nullptr;
        lv_color_t *humTriBuf = nullptr;
        lv_obj_t *timeLabel = nullptr;
        // hard character budget for the name (monospace advance ~= 0.6 * px);
        // guarantees no wrap even if LV long-mode ever misbehaves.
        size_t maxNameChars = 0;
        std::string cacheName, cacheTemp, cacheHum, cacheBatt, cacheTime;
        int cacheRssiDbm = -1000; // redraw text only on >=2 dB change (churn)
        bool cacheStale = false;
        int cacheBattLvl = -1;
    };

    static void buildGrid();
    static lv_font_t *fontFor(int px);
    static void clearAll();

    static DashState s_state;
    static lv_obj_t *s_root;
    static lv_obj_t *s_statusLabel;
    static lv_obj_t *s_ipLabel; // dashboard status bar, right of clock
    // true while any full rebuild holds the LVGL lock (apply/setup/needs-
    // config screens); refresh() skips its cycle then instead of fighting
    // for objects mid-rebuild. Plain bool is enough (stale read = one
    // skipped 1s cycle at worst).
    static volatile bool s_applying;
    static std::vector<Cell> s_cells;
    static AppConfig s_cfg;
};