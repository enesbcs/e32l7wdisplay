#include "Dashboard.hpp"
#include "Sensor.hpp"
#include "PinConfig.hpp"
#include "LogBuffer.hpp"
#include "esp_lvgl_port.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "CH422G.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// Plain-object icon kit: same pipeline as labels/containers (no canvas,
// no blit, no stride questions). All coordinates are parent-relative.
static lv_obj_t *iconBox(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mkDisc(lv_obj_t *parent, int x, int y, int d, lv_color_t c) {
    lv_obj_t *o = iconBox(parent, x, y, d, d);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

static lv_obj_t *mkRing(lv_obj_t *parent, int cx, int cy, int r, int w, lv_color_t c) {
    lv_obj_t *o = iconBox(parent, cx - r, cy - r, 2 * r, 2 * r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_color(o, c, 0);
    return o;
}

static lv_obj_t *mkBar(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c, int rad = 0) {
    lv_obj_t *o = iconBox(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, rad, 0);
    return o;
}

static void buildTempIcon(lv_obj_t *root, int W, int H, lv_color_t c) {
    int D = H / 3;
    if (D > W - 2) D = W - 2;
    if (D < 8) D = 8;
    int bulbCy = H - D / 2 - 1;
    int sw = W / 6;
    if (sw < 3) sw = 3;
    int stemX = (W - sw) / 2;
    mkDisc(root, (W - D) / 2, H - D, D, c); // bulb
    mkBar(root, stemX, 2, sw, bulbCy - 2 + 2, c, sw / 2); // stem into bulb
    for (int k = 0; k < 3; k++) // scale ticks
        mkBar(root, stemX - 5, 8 + k * ((bulbCy - 12) / 3), 4, 2, c, 0);
}

static void buildHumIcon(lv_obj_t *root, int W, int H, lv_color_t c,
                           lv_color_t **triBufOut) {
    // teardrop: bottom disc (object) + top triangle on a small canvas.
    // Canvas uses only proven layer primitives (triangle/fill, no transform:
    // the rotated diamond sent the SW transform renderer into a 30s+ stall).
    int D = H / 2;
    if (D > W - 2) D = W - 2;
    if (D < 8) D = 8;
    int discY = H - D;
    mkDisc(root, (W - D) / 2, discY, D, c);
    int TW = D - 2;
    if (TW < 6) TW = 6;
    // deep overlap to the disc center: the wide base gets buried inside the
    // disc, so no black notch can remain at the junction
    int TH = discY + D / 2;
    if (TH < 4) TH = 4;
    lv_obj_t *tri = lv_canvas_create(root);
    // ARGB8888 (not RGB565): RGB565 has no alpha channel, so the TRANSP
    // fill below rendered as opaque black - a dark fringe where the canvas
    // overlaps the disc (the mirror's CSS triangle is truly transparent).
    size_t nbytes = (size_t)TW * TH * 4;
    uint8_t *tbuf = (uint8_t *)heap_caps_calloc(
        nbytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tbuf) tbuf = (uint8_t *)calloc(nbytes, 1);
    if (tbuf) {
        lv_canvas_set_buffer(tri, tbuf, TW, TH, LV_COLOR_FORMAT_ARGB8888);
        if (triBufOut) *triBufOut = (lv_color_t *)tbuf;
    }
    lv_obj_set_pos(tri, (W - TW) / 2, 0);
    if (tbuf) {
        lv_canvas_fill_bg(tri, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(tri, &layer);
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.color = c;
        td.p[0].x = TW / 2; td.p[0].y = 0;
        td.p[1].x = 0;      td.p[1].y = TH - 1;
        td.p[2].x = TW - 1; td.p[2].y = TH - 1;
        lv_draw_triangle(&layer, &td);
        lv_canvas_finish_layer(tri, &layer);
    }
}

static lv_obj_t *buildBattIcon(lv_obj_t *root, int W, int H, lv_color_t c) {
    (void)W;
    // 20x18 outline + nub; inner fill bar returned for level updates
    lv_obj_t *box = iconBox(root, 0, 0, 20, 18);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, c, 0);
    lv_obj_set_style_radius(box, 2, 0);
    mkBar(root, 19, 6, 3, 6, c, 1); // nub
    return mkBar(root, 2, 2, 0, 14, c, 0); // fill, width set from level
}

static lv_obj_t *s_wifiRings[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *s_wifiDot = nullptr;
static int s_wifiLevel = -2; // icon-table row (-1 = unknown, -2 = force repaint)

static void buildWifiObjects(lv_obj_t *root) {
    // 36x30 fan at top-right: clip box shows only the top arcs.
    // Rings + dot are kept for the RSSI level painter below.
    const int ox = 800 - 16 - 36, oy = 4;
    lv_obj_t *clip = iconBox(root, ox, oy, 36, 20);
    s_wifiRings[0] = mkRing(clip, 18, 16, 5, 2, lv_color_white());
    s_wifiRings[1] = mkRing(clip, 18, 16, 10, 2, lv_color_white());
    s_wifiRings[2] = mkRing(clip, 18, 16, 15, 2, lv_color_white());
    s_wifiDot = mkDisc(root, ox + 15, oy + 21, 6, lv_color_white());
    s_wifiLevel = -2;
}

// RSSI -> icon-table row 4..0 (-1 = unknown). Rows 3-4 share the 1-arc
// shape and differ in color only (yellow vs orange).
static int wifiLevelFor(int dbm) {
    if (dbm >= -50) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -75) return 2;
    if (dbm >= -85) return 1;
    return 0;
}

// STA RSSI, valid only with a live STA address
static bool readWifiRssi(int &dbm) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0)
        return false;
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) return false;
    dbm = rssi;
    return true;
}

// repaints only on level change (no per-second redraw churn)
static void updateWifiIcon() {
    int dbm = 0;
    int lvl = readWifiRssi(dbm) ? wifiLevelFor(dbm) : -1;
    if (lvl == s_wifiLevel) return;
    s_wifiLevel = lvl;
    int arcs = 0;
    lv_color_t col = lv_color_make(120, 120, 120); // unknown: gray dot
    switch (lvl) {
        case 4: arcs = 3; col = lv_color_make(80, 220, 120); break;
        case 3: arcs = 2; col = lv_color_make(80, 220, 120); break;
        case 2: arcs = 1; col = lv_color_make(235, 200, 90); break;
        case 1: arcs = 1; col = lv_color_make(235, 150, 70); break;
        case 0: arcs = 0; col = lv_color_make(235, 90, 90); break;
        default: break;
    }
    for (int i = 0; i < 3; i++) {
        if (!s_wifiRings[i]) continue;
        if (i < arcs) lv_obj_clear_flag(s_wifiRings[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_wifiRings[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(s_wifiRings[i], col, 0);
    }
    if (s_wifiDot) lv_obj_set_style_bg_color(s_wifiDot, col, 0);
}

static constexpr int OUTER_MARGIN = 16;
static constexpr int TOP_BAR = 34;
static constexpr int GAP = 12;

DashState Dashboard::s_state = DashState::NONE;
lv_obj_t *Dashboard::s_root = nullptr;
lv_obj_t *Dashboard::s_statusLabel = nullptr;
lv_obj_t *Dashboard::s_ipLabel = nullptr;
volatile bool Dashboard::s_applying = false;
// status-bar caches ("\x01" = impossible value, forces first paint);
// labels are only rewritten on change to avoid re-raster + flush churn
static std::string s_timeCache = "\x01";
static std::string s_ipCache = "\x01";
std::vector<Dashboard::Cell> Dashboard::s_cells;

AppConfig Dashboard::s_cfg;


// Intentionally kept although currently caller-free: generic backlight
// switch for future idle-dim/OTA-blank use (OTA itself runs in safeboot).
static bool s_displayOn = true;

void Dashboard::blankDisplay(bool off) {
    s_displayOn = !off;
    CH422G::setBacklight(!off);
}

bool Dashboard::isDisplayOn() { return s_displayOn; }

#if __has_include("driver/temperature_sensor.h")
#include "driver/temperature_sensor.h"
#define HAVE_MCU_TEMP 1
#else
#define HAVE_MCU_TEMP 0
#endif

#if HAVE_MCU_TEMP
static temperature_sensor_handle_t s_tsens = nullptr;
#endif

void Dashboard::initMcuTemp() {
#if HAVE_MCU_TEMP
    if (s_tsens) return;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || !s_tsens) {
        s_tsens = nullptr;
        LogBuffer::logf("MCU temp sensor unavailable");
        return;
    }
    if (temperature_sensor_enable(s_tsens) != ESP_OK) {
        s_tsens = nullptr;
        LogBuffer::logf("MCU temp sensor enable failed");
    }
#else
    LogBuffer::logf("MCU temp sensor: no driver");
#endif
}

bool Dashboard::readMcuTempC(float &out) {
#if HAVE_MCU_TEMP
    if (!s_tsens) return false;
    return temperature_sensor_get_celsius(s_tsens, &out) == ESP_OK;
#else
    (void)out;
    return false;
#endif
}

static std::string currentIpText() {
    esp_netif_ip_info_t ip = {};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        char b[32];
        snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));
        return b;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr != 0) {
        char b[32];
        snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));
        return b;
    }
    return "192.168.4.1";
}

// Pre-converted static UbuntuMono bitmaps (src/fonts/, lv_font_conv):
// zero runtime TTF rasterization -> the stbtt assert class is gone.
// Exact table covers every size buildGrid can request; nearest-size
// fallback + LV_FONT_DEFAULT guarantee a non-null return.
LV_FONT_DECLARE(ubuntu_18);
LV_FONT_DECLARE(ubuntu_19);
LV_FONT_DECLARE(ubuntu_22);
LV_FONT_DECLARE(ubuntu_24);
LV_FONT_DECLARE(ubuntu_28);
LV_FONT_DECLARE(ubuntu_32);
LV_FONT_DECLARE(ubuntu_33);
LV_FONT_DECLARE(ubuntu_36);
LV_FONT_DECLARE(ubuntu_42);
LV_FONT_DECLARE(ubuntu_44);
LV_FONT_DECLARE(ubuntu_46);
LV_FONT_DECLARE(ubuntu_52);

static const struct { int px; const lv_font_t *f; } s_staticFonts[] = {
    {18, &ubuntu_18}, {19, &ubuntu_19}, {22, &ubuntu_22}, {24, &ubuntu_24},
    {28, &ubuntu_28}, {32, &ubuntu_32}, {33, &ubuntu_33}, {36, &ubuntu_36},
    {42, &ubuntu_42}, {44, &ubuntu_44}, {46, &ubuntu_46}, {52, &ubuntu_52},
};

lv_font_t *Dashboard::fontFor(int px) {
    const lv_font_t *best = nullptr;
    int bestDiff = 1000;
    for (auto &e : s_staticFonts) {
        int d = e.px >= px ? e.px - px : (px - e.px) * 2; // prefer exact, then larger
        if (d < bestDiff) {
            bestDiff = d;
            best = e.f;
        }
    }
    if (!best) best = LV_FONT_DEFAULT;
    return (lv_font_t *)best;
}

void Dashboard::begin() {
    // bounded lock with retries: an LVGL stall must not brick boot
    // (the WDT is not armed this early) - fail-open without display
    bool locked = false;
    for (int i = 0; i < 15; i++) {
        if (lvgl_port_lock(2000)) { locked = true; break; }
        LogBuffer::logf("FATAL: no LVGL lock (try %d)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!locked) {
        LogBuffer::logf("FATAL: display unavailable, continuing headless");
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        LogBuffer::logf("FATAL: no LVGL screen");
        lvgl_port_unlock();
        return;
    }
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, DISPLAY::H_RES, DISPLAY::V_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);

    s_statusLabel = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_statusLabel, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_statusLabel, fontFor(18), 0);
    lv_label_set_text(s_statusLabel, "");
    lv_obj_align(s_statusLabel, LV_ALIGN_TOP_LEFT, OUTER_MARGIN, 6);

    buildWifiObjects(s_root);

    // boot splash: the staged boot leaves seconds of black before the first
    // applyConfig; reassure until the grid/setup takes over (wiped by their
    // lv_obj_clean). Existing 28px font, no state to maintain.
    lv_obj_t *boot = lv_label_create(s_root);
    lv_obj_set_style_text_color(boot, lv_color_white(), 0);
    lv_obj_set_style_text_font(boot, fontFor(28), 0);
    lv_obj_set_style_text_align(boot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(boot, DISPLAY::H_RES - 2 * OUTER_MARGIN);
    lv_label_set_text(boot, "Loading...");
    lv_obj_align(boot, LV_ALIGN_CENTER, 0, 0);

    s_state = DashState::NONE;
    lvgl_port_unlock();
}

void Dashboard::clearAll() {
    // ORDERING CONSTRAINT: callers must lv_obj_clean(s_root) FIRST (destroy
    // the canvas widgets while their buffers are still allocated) and only
    // then clearAll(). Freeing buffers first leaves dangling draw_buf
    // pointers behind: the canvas destructor touches them
    // (lv_image_cache_drop) -> PSRAM heap corruption -> later hangs
    // (observed: main stuck in lv_label_set_text, WDT reboot on rapid
    // grid-size switching).
    for (auto &c : s_cells) {
        if (c.humTriBuf) heap_caps_free(c.humTriBuf);
    }
    s_cells.clear();
}

bool Dashboard::applyConfig(const AppConfig &config) {
    if (!s_root) {
        LogBuffer::logf("applyConfig: no display, skipped");
        return false;
    }
    s_applying = true;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    s_cfg = config;
    // restore persisted sensor name overrides (stored as "id\x1fname" pairs)
    for (auto &ov : config.name_override_ids) {
        size_t p = ov.find('\x1f');
        if (p != std::string::npos && p > 0 && p + 1 < ov.size())
            SensorRegistry::setDisplayName(ov.substr(0, p), ov.substr(p + 1));
    }
    if (!lvgl_port_lock(1000)) {
        LogBuffer::logf("applyConfig: LVGL busy, skipped");
        s_applying = false;
        return false;
    }
    // flush pending layouts FIRST: label set_text() registers a display-level
    // UPDATE_LAYOUT_COMPLETED callback holding a raw object pointer; cleaning
    // with layouts pending leaves it firing on freed objects later
    // (use-after-free -> main stuck in set_text, WDT). Seen after APPLYs.
    lv_refr_now(lv_disp_get_default());
    lv_obj_clean(s_root);
    clearAll();
    // recreate status bar (clock + IP + wifi icon) on fresh root
    s_statusLabel = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_statusLabel, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_statusLabel, fontFor(18), 0);
    lv_label_set_text(s_statusLabel, "");
    lv_obj_align(s_statusLabel, LV_ALIGN_TOP_LEFT, OUTER_MARGIN, 6);
    s_ipLabel = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_ipLabel, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_ipLabel, fontFor(18), 0);
    lv_label_set_text(s_ipLabel, "");
    // right edge 8px left of the 36px wifi icon (icon starts at 800-16-36)
    lv_obj_align(s_ipLabel, LV_ALIGN_TOP_RIGHT, -(16 + 36 + 8), 6);
    s_timeCache = "\x01";
    s_ipCache = "\x01";

    buildWifiObjects(s_root);

    bool anyPopulated = false;
    for (auto &c : config.cells) if (!c.empty()) anyPopulated = true;

    if (config.cells.empty() || !anyPopulated) {
        s_state = DashState::NEEDS_CONFIG;
        showNeedsConfigScreen();
    } else {
        s_state = DashState::DASHBOARD;
        buildGrid();
    }
    // DIAG: catch heap corruption right after the heavy rebuild churn
    if (!heap_caps_check_integrity_all(true))
        LogBuffer::logfSerial("HEAP INTEGRITY FAIL (after applyConfig)");
    LogBuffer::logfSerial("APPLY done %ums",
        (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t0));
    s_applying = false;
    lvgl_port_unlock();
    return true;
}

void Dashboard::buildGrid() {
    int rows = s_cfg.grid_rows, cols = s_cfg.grid_cols;
    int cellW = (DISPLAY::H_RES - 2 * OUTER_MARGIN - (cols - 1) * GAP) / cols;
    int gridH = DISPLAY::V_RES - TOP_BAR - OUTER_MARGIN;
    int cellH = (gridH - (rows - 1) * GAP) / rows;

    // fit-first fonts: value rows share what remains after the fixed
    // name/time/margin overhead. (The old cellH/3 rule overshot small cells:
    // 3x1 rendered 45px values into 90px of content, rows spilled past the
    // container bottom.)
    int fxName = cellH / 4;
    if (fxName < 16) fxName = 16;
    if (fxName > 36) fxName = 36;
    int fxSmall = cellH / 7;
    if (fxSmall < 12) fxSmall = 12;
    if (fxSmall > 22) fxSmall = 22;
    int battH = fxSmall + 4;

    lv_font_t *fName = fontFor(fxName);
    lv_font_t *fSmall = fontFor(fxSmall);

    s_cells.reserve(rows * cols);
    int fxValueLast = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            size_t idx = (size_t)r * cols + c;
            std::string id = (idx < s_cfg.cells.size()) ? s_cfg.cells[idx] : "";
            if (id.empty()) continue;

            Cell cell;
            cell.sensorId = id;
            cell.maxNameChars = (size_t)((cellW - 8) / (fxName * 6 / 10));

            cell.container = lv_obj_create(s_root);
            lv_obj_set_size(cell.container, cellW, cellH);
            lv_obj_set_pos(cell.container, OUTER_MARGIN + c * (cellW + GAP),
                           TOP_BAR + r * (cellH + GAP));
            lv_obj_set_style_border_width(cell.container, 1, 0);
            lv_obj_set_style_border_color(cell.container, lv_color_make(60, 60, 60), 0);
            lv_obj_set_style_radius(cell.container, 6, 0);
            lv_obj_set_style_bg_color(cell.container, lv_color_make(18, 18, 18), 0);
            lv_obj_set_style_bg_opa(cell.container, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(cell.container, 0, 0);
            lv_obj_clear_flag(cell.container, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(cell.container, LV_SCROLLBAR_MODE_OFF);

            int y = 6;
            // name
            cell.nameLabel = lv_label_create(cell.container);
            lv_obj_set_style_text_font(cell.nameLabel, fName, 0);
            lv_obj_set_style_text_color(cell.nameLabel, lv_color_white(), 0);
            lv_obj_set_style_text_align(cell.nameLabel, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(cell.nameLabel, cellW - 8);
            // NO LV long-mode here: the C++ truncation above already guarantees
            // fit, and the DOT engine hung the device (decoded WDT backtrace).
            lv_obj_set_pos(cell.nameLabel, 4, y);
            lv_label_set_text(cell.nameLabel, "");

            y += fxName + 6;
            int contentH = cellH - y;

            // value rows: fxValue is solved from the space left after the
            // time row + margins, so rows can never spill past the cell.
            // Prefer the 3-row (T/H + batt) layout when it stays readable.
            int avail = contentH - (fxSmall + 6) - 6;
            if (avail < 32) avail = 32;
            int nRows, fxValue;
            int v3 = (avail - battH) / 2 - 8;
            if (v3 >= 20) { nRows = 3; fxValue = v3; }
            else { nRows = 2; fxValue = avail / 2 - 8; }
            if (fxValue < 16) fxValue = 16;
            if (fxValue > 52) fxValue = 52;
            int rowH = fxValue + 8;
            // shrink until the value row fits the cell width (monospace!)
            while (fxValue > 16 && (7 * fxValue + 44) > cellW) {
                fxValue -= 2;
                rowH = fxValue + 8;
            }
            int used = nRows * rowH + (nRows == 3 ? battH : 0);
            int extra = (avail - used) / (nRows + 1);
            if (extra < 0) extra = 0;

            lv_font_t *fVal = fontFor(fxValue);
            int iconW = (fxValue < 36) ? 24 : 30;
            int iconH = fxValue + 8;
            fxValueLast = fxValue;

            // explicit width + clip: auto-sized labels stayed invisible.
            // rsv reserves room at the right edge (compact rows share it
            // with a right-aligned value).
            auto makeLabel = [&](lv_font_t *font, lv_color_t color, int x, int y,
                                 int rsv = 0) {
                lv_obj_t *label = lv_label_create(cell.container);
                lv_obj_set_style_text_font(label, font, 0);
                lv_obj_set_style_text_color(label, color, 0);
                lv_obj_set_pos(label, x, y);
                lv_obj_set_width(label, cellW - x - rsv);
                lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
                lv_label_set_text(label, "");
                return label;
            };
            // compact rows (2 value rows, i.e. 3-row grids): no room for a
            // battery row, so battery shares the temperature row and RSSI
            // the humidity row, right-aligned. Icons join in when the cell
            // is wide enough for icon+text beside the value (3x1/3x2).
            const bool compact = (nRows == 2);
            const bool compactIcons = compact && (cellW >= 220);
            // shared icon column: both icons start at the same X, both
            // texts end at the same edge (narrow cells stay text-only)
            const int battTW = compactIcons ? 80 : 58;
            const int rssiTW = 80;
            const int iconX = cellW - 8 - rssiTW - 4 - 18;

            // broadcast tower: mast + two top arcs in an 18x14 box
            auto buildRssiIcon = [&](lv_obj_t *root, lv_color_t color) {
                mkBar(root, 8, 7, 3, 7, color, 1); // mast
                lv_obj_t *clip = iconBox(root, 0, 0, 18, 9);
                mkRing(clip, 9, 12, 8, 1, color);
                mkRing(clip, 9, 12, 5, 1, color);
            };

            // value icons sit ~5px above their number baseline
            y += extra;
            {
                cell.tempIconRoot = iconBox(cell.container, 6, y - 5, iconW, iconH);
                buildTempIcon(cell.tempIconRoot, iconW, iconH, lv_color_make(255, 190, 90));
                int rsv = compact ? (compactIcons ? 20 + 4 : 0) + battTW + 8 : 0;
                cell.tempLabel = makeLabel(fVal, lv_color_make(255, 190, 90),
                                           6 + iconW + 4, y - 2, rsv);
                if (compact) {
                    if (compactIcons) {
                        cell.battIconRoot = iconBox(cell.container,
                            iconX, y - 2, 20, 18);
                        cell.battFill = buildBattIcon(cell.battIconRoot, 22, 18,
                            lv_color_make(150, 220, 140));
                        float lvl = -1.0f;
                        SensorReading rr;
                        if (SensorRegistry::get(id, rr) && rr.hasBattery)
                            lvl = rr.battery;
                        if (lvl >= 0) {
                            cell.cacheBattLvl = (int)(lvl + 0.5f);
                            lv_obj_set_width(cell.battFill, (16 * cell.cacheBattLvl) / 100);
                        }
                    }
                    cell.battLabel = makeLabel(fSmall, lv_color_make(150, 220, 140),
                                               cellW - 8 - battTW, y - 2, 8);
                    lv_obj_set_style_text_align(cell.battLabel, LV_TEXT_ALIGN_RIGHT, 0);
                }
            }
            y += rowH + extra;
            {
                cell.humIconRoot = iconBox(cell.container, 6, y - 7, iconW, iconH);
                buildHumIcon(cell.humIconRoot, iconW, iconH, lv_color_make(110, 190, 255), &cell.humTriBuf);
                int rsv = compact ? (compactIcons ? 18 + 4 : 0) + rssiTW + 8 : 0;
                cell.humLabel = makeLabel(fVal, lv_color_make(110, 190, 255),
                                          6 + iconW + 4, y - 2, rsv);
                if (compact) {
                    if (compactIcons) {
                        cell.rssiIconRoot = iconBox(cell.container,
                            iconX, y - 2, 18, 14);
                        buildRssiIcon(cell.rssiIconRoot, lv_color_make(154, 160, 166));
                    }
                    cell.rssiLabel = makeLabel(fSmall, lv_color_make(154, 160, 166),
                                               cellW - 8 - rssiTW, y - 2, 8);
                    lv_obj_set_style_text_align(cell.rssiLabel, LV_TEXT_ALIGN_RIGHT, 0);
                }
            }

            // narrow cells (<213px, i.e. 4-column grids): icon + full texts
            // cannot fit beside the RSSI cluster, so the battery icon is
            // dropped and batt/rssi share 18px text (as in compact rows)
            const bool narrowBatt = (cellW < 213);
            lv_font_t *fBatt = narrowBatt ? fontFor(18) : fSmall;
            if (nRows == 3) {
                y += rowH + extra;
                if (!narrowBatt) {
                    cell.battIconRoot = iconBox(cell.container, 6, y, 22, 18);
                    cell.battFill = buildBattIcon(cell.battIconRoot, 22, 18, lv_color_make(150, 220, 140));
                    float lvl = -1.0f;
                    SensorReading rr;
                    if (SensorRegistry::get(id, rr) && rr.hasBattery) lvl = rr.battery;
                    if (lvl >= 0) {
                        cell.cacheBattLvl = (int)(lvl + 0.5f);
                        lv_obj_set_width(cell.battFill, (16 * cell.cacheBattLvl) / 100);
                    }
                }
                cell.battLabel = makeLabel(fBatt, lv_color_make(150, 220, 140),
                                           narrowBatt ? 6 : 6 + 22 + 4, y - 2,
                                           narrowBatt ? cellW - 69 : 0);
                // RSSI at the right end of the battery row (wide + noclip:
                // "-100 dB" must never wrap onto the next line)
                cell.rssiIconRoot = iconBox(cell.container, cellW - 104, y, 18, 14);
                buildRssiIcon(cell.rssiIconRoot, lv_color_make(154, 160, 166));
                cell.rssiLabel = lv_label_create(cell.container);
                lv_obj_set_style_text_font(cell.rssiLabel, fBatt, 0);
                lv_obj_set_style_text_color(cell.rssiLabel, lv_color_make(154, 160, 166), 0);
                lv_obj_set_style_text_align(cell.rssiLabel, LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_set_width(cell.rssiLabel, 76);
                lv_obj_set_pos(cell.rssiLabel, cellW - 82, y - 2);
                lv_label_set_long_mode(cell.rssiLabel, LV_LABEL_LONG_CLIP);
                lv_label_set_text(cell.rssiLabel, "");
            }

            cell.timeLabel = lv_label_create(cell.container);
            lv_obj_set_style_text_font(cell.timeLabel, fSmall, 0);
            lv_obj_set_style_text_color(cell.timeLabel, lv_color_make(110, 110, 110), 0);
            lv_obj_set_style_text_align(cell.timeLabel, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(cell.timeLabel, cellW - 8);
            lv_obj_set_pos(cell.timeLabel, 4, cellH - fxSmall - 6);
            lv_label_set_text(cell.timeLabel, "");

            s_cells.push_back(std::move(cell));
        }
    }
    LogBuffer::logf("Dashboard grid built: %dx%d, %d cells, font name=%d value=%d",
                    rows, cols, s_cells.size(), fxName, fxValueLast);
}

void Dashboard::showSetupScreen(const std::string &ssid, const std::string &ip) {
    s_state = DashState::SETUP;
    if (!s_root) {
        LogBuffer::logf("showSetupScreen: no display, skipped");
        return;
    }
    s_applying = true;
    if (!lvgl_port_lock(1000)) {
        LogBuffer::logf("showSetupScreen: LVGL busy, skipped");
        s_applying = false;
        return;
    }
    lv_refr_now(lv_disp_get_default());
    lv_obj_clean(s_root);
    clearAll();
    s_statusLabel = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_statusLabel, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_statusLabel, fontFor(18), 0);
    lv_label_set_text(s_statusLabel, "");
    lv_obj_align(s_statusLabel, LV_ALIGN_TOP_LEFT, OUTER_MARGIN, 6);

    buildWifiObjects(s_root);

    lv_obj_t *lbl = lv_label_create(s_root);
    std::string txt = "Setup mode\n\n"
                      "Connect to WiFi: " + ssid + "\n\n"
                      "Then open:\n" + ip +
                      "\n\nGo to WiFi / Data Source / Dashboard";
    lv_obj_set_style_text_font(lbl, fontFor(24), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, DISPLAY::H_RES - 2 * OUTER_MARGIN);
    lv_label_set_text(lbl, txt.c_str());
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    s_applying = false;
    lvgl_port_unlock();
}

void Dashboard::showNeedsConfigScreen() {
    s_state = DashState::NEEDS_CONFIG;
    if (!s_root) {
        LogBuffer::logf("showNeedsConfigScreen: no display, skipped");
        return;
    }
    s_applying = true;
    if (!lvgl_port_lock(1000)) {
        LogBuffer::logf("showNeedsConfigScreen: LVGL busy, skipped");
        s_applying = false;
        return;
    }
    lv_refr_now(lv_disp_get_default());
    lv_obj_clean(s_root);
    clearAll();
    s_statusLabel = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_statusLabel, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_statusLabel, fontFor(18), 0);
    lv_label_set_text(s_statusLabel, "");
    lv_obj_align(s_statusLabel, LV_ALIGN_TOP_LEFT, OUTER_MARGIN, 6);

    buildWifiObjects(s_root);

    lv_obj_t *lbl = lv_label_create(s_root);
    std::string txt = "No sensors configured\n\nPlease open the web UI and\nconfigure Data Source + Dashboard";
    lv_obj_set_style_text_font(lbl, fontFor(28), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, DISPLAY::H_RES - 2 * OUTER_MARGIN);
    std::string ip = currentIpText();
    if (!ip.empty()) {
        txt += "\n\nConnect to:   " + ip;
    }
    lv_label_set_text(lbl, txt.c_str());
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    s_applying = false;
    lvgl_port_unlock();
}

void Dashboard::refresh(uint32_t nowMs) {
    if (s_state != DashState::DASHBOARD) return;
    // a rebuild in progress owns the objects; touch nothing (not even the
    // lock) until it finishes - one skipped 1s cycle at worst
    if (s_applying) return;
    // bounded lock: a stalled LVGL task must never wedge the main loop
    // (that ends in a task-watchdog reboot with no evidence left behind)
    static uint32_t lockFails = 0;
    static uint32_t lastAlive = 0;
    if (!lvgl_port_lock(200)) {
        lockFails++;
        LogBuffer::logfSerial("REFRESH lock fail #%u", (unsigned)lockFails);
        return;
    }
    for (auto &cell : s_cells) {
        // age ticker: ALWAYS runs, from the cell-local data timestamp, with
        // no registry lock and no allocations (SSO strings only). The
        // generation gate below skips values/names only - gating this too
        // froze every row at "just now" (and never tripped stale styling).
        {
            std::string tstr;
            bool stale = false;
            if (cell.lastDataMs != 0) {
                char buf[24];
                // guard: an upsert stamped by another task AFTER main sampled
                // now (same loop iteration) would wrap to ~1193h + red for one
                // cycle; clamp like the /dashboard mirror already does
                uint32_t age = (nowMs >= cell.lastDataMs) ? (nowMs - cell.lastDataMs) : 0;
                if (age < 90 * 1000) {
                    tstr = "just now";
                } else if (age < 3600 * 1000) {
                    snprintf(buf, sizeof(buf), "%u min ago", (unsigned)(age / 60000));
                    tstr = buf;
                } else {
                    snprintf(buf, sizeof(buf), "%u h ago", (unsigned)(age / 3600000));
                    tstr = buf;
                }
                stale = (age > 3600 * 1000);
            }
            if (tstr != cell.cacheTime) {
                if (tstr.size() <= 64) { lv_label_set_text(cell.timeLabel, tstr.c_str());; }
                cell.cacheTime = tstr;
            }
            // NOTE: lv_obj_set_style_* ALWAYS invalidates (even with identical
            // values), so setting them every second forced a full-cell redraw +
            // PSRAM framebuffer flush at 1 Hz - visible as a jump while the RGB
            // DMA scanout contends with other PSRAM traffic. Set once at build
            // (dark) and only on stale transitions from here.
            if (stale != cell.cacheStale) {
                cell.cacheStale = stale;
                if (stale) {
                    lv_obj_set_style_bg_color(cell.container, lv_color_make(200, 30, 30), 0);
                    lv_obj_set_style_text_color(cell.nameLabel, lv_color_white(), 0);
                } else {
                    lv_obj_set_style_bg_color(cell.container, lv_color_make(18, 18, 18), 0);
                    lv_obj_set_style_text_color(cell.nameLabel, lv_color_white(), 0);
                }
               ;
            }
        }
        // generation short-circuit: unchanged rows cost one lock + one int
        // compare (no strings, no second lookup). Missing rows (gen 0)
        // always fall through to the existing empty-handling below.
        uint32_t gen = SensorRegistry::getGeneration(cell.sensorId);
        if (gen != 0 && gen == cell.lastGeneration) continue;
        SensorSnapshot snap;
        bool have = SensorRegistry::getSnapshot(cell.sensorId, snap) && snap.found;
        // reset on miss: a reappearing entry restarts generation at 1, which
        // could otherwise equal a stale cached value and stick forever
        if (have) cell.lastGeneration = snap.reading.generation;
        else cell.lastGeneration = 0;
        SensorReading r;
        if (have) r = snap.reading;

        // content guard: a corrupt std::string (wild length) would send the
        // text pipeline crawling gigabytes instead of failing fast
        auto sane = [&](const std::string &s, const char *what) {
            if (s.size() > 64) {
                LogBuffer::logfSerial("REFRESH insane %s len=%u cell=%s", what,
                    (unsigned)s.size(), cell.sensorId.c_str());
                return false;
            }
            return true;
        };
        std::string name = have ? snap.name : cell.sensorId;
        // hard cut: never let the name wrap onto the value rows
        if (cell.maxNameChars > 3 && name.size() > cell.maxNameChars) {
            size_t n = cell.maxNameChars - 1;
            while (n > 0 && (name[n] & 0xC0) == 0x80) n--; // keep UTF-8 intact
            name = name.substr(0, n) + "…";
        }
        if (name != cell.cacheName) {
            if (sane(name, "name")) { lv_label_set_text(cell.nameLabel, name.c_str());; }
            cell.cacheName = name;
        }

        // entry existence (!= data): RSSI/name stubs have no T/H/B.
        // Time/stale/placeholders follow real data only (mirror parity).
        bool haveData = have && (r.hasTemperature || r.hasHumidity || r.hasBattery);
        std::string temp, hum, batt;
        if (haveData) {
            char buf[24];
            if (r.hasTemperature) {
                snprintf(buf, sizeof(buf), "%.1f C", r.temperature);
                temp = buf;
            }
            if (r.hasHumidity) {
                snprintf(buf, sizeof(buf), "%d %%", (int)(r.humidity + 0.5f));
                hum = buf;
            }
            if (r.hasBattery) {
                snprintf(buf, sizeof(buf), "%d %%", (int)(r.battery + 0.5f));
                batt = buf;
            }
            // remember the data timestamp for the lock-free age ticker above
            // (the ticker, not this gated path, owns the time label)
            cell.lastDataMs = r.lastUpdateMillis;
        } else {
            // no data yet: keep rows empty (no placeholders)
            temp = "";
            hum = "";
            batt = "";
            cell.lastDataMs = 0;
        }

        if (temp != cell.cacheTemp) {
            if (sane(temp, "temp")) { lv_label_set_text(cell.tempLabel, temp.c_str());; }
            cell.cacheTemp = temp;
        }
        if (hum != cell.cacheHum) {
            if (sane(hum, "hum")) { lv_label_set_text(cell.humLabel, hum.c_str());; }
            cell.cacheHum = hum;
        }
        // NOTE: battLabel exists only in the 3-row layout (nRows==3); in
        // 2-row cells it is NULL. An unguarded set_text(NULL) trips
        // LV_ASSERT_OBJ -> LV_ASSERT_HANDLER=while(1) (lv_conf.h) -> main
        // spins with the LVGL lock held -> WDT reboot. Seen on 3-row grids
        // with battery-reporting sensors (backtrace: refresh:612).
        if (batt != cell.cacheBatt) {
            if (cell.battLabel && sane(batt, "batt")) { lv_label_set_text(cell.battLabel, batt.c_str());; }
            cell.cacheBatt = batt;
        }
        if (r.hasBattery && cell.battFill) {
            int lvl = (int)(r.battery + 0.5f);
            if (lvl != cell.cacheBattLvl) {
                cell.cacheBattLvl = lvl;
                lv_obj_set_width(cell.battFill, (16 * lvl) / 100);
               ;
            }
        }

        // icons only for data the device actually reported
        auto showIf = [](lv_obj_t *o, bool show) {
            if (!o) return;
            if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        };
        showIf(cell.tempIconRoot, have && r.hasTemperature);
        showIf(cell.humIconRoot, have && r.hasHumidity);
        showIf(cell.battIconRoot, have && r.hasBattery);
        bool hasR = have && r.hasRssi;
        showIf(cell.rssiIconRoot, hasR);
        showIf(cell.rssiLabel, hasR);
        if (hasR && cell.rssiLabel) {
            // hysteresis: ±1 dB flutter would re-rasterize every advert
            if (abs(r.rssiDbm - cell.cacheRssiDbm) >= 2) {
                cell.cacheRssiDbm = r.rssiDbm;
                char rbuf[24];
                snprintf(rbuf, sizeof(rbuf), "%d dB", r.rssiDbm);
                lv_label_set_text(cell.rssiLabel, rbuf);
               ;
            }
        }
    }
    if (nowMs - lastAlive > 30000) {
        lastAlive = nowMs;
        std::string t0 = s_cells.empty() ? "-" : s_cells[0].cacheTemp;
        LogBuffer::logfSerial("REFRESH alive cells=%u t0='%s' lockFails=%u",
            (unsigned)s_cells.size(), t0.c_str(), (unsigned)lockFails);
    }
    // wifi fan level, 5s cadence is plenty for RSSI (repaint-on-change only)
    static uint32_t lastWifiUpd = 0;
    if (nowMs - lastWifiUpd >= 5000) {
        lastWifiUpd = nowMs;
        updateWifiIcon();
    }
    // status bar: YYYY-MM-DD HH:MM top-left (NTP-synced only, else empty),
    // STA/AP IP top-right. Text set only on change (no re-raster churn).
    if (s_statusLabel) {
        char tbuf[20] = {0};
        time_t now = time(nullptr);
        if (now > 1700000000L) {
            struct tm tmv;
            int Y, M, D, h, m;
            if (localtime_r(&now, &tmv)
                && (Y = tmv.tm_year + 1900) >= 2024 && Y <= 2100
                && (M = tmv.tm_mon + 1) >= 1 && M <= 12
                && (D = tmv.tm_mday) >= 1 && D <= 31
                && (h = tmv.tm_hour) >= 0 && h < 24
                && (m = tmv.tm_min) >= 0 && m < 60) {
                snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d",
                         Y, M, D, h, m);
            }
        }
        if (s_timeCache != tbuf) {
            s_timeCache = tbuf;
            lv_label_set_text(s_statusLabel, tbuf);
        }
    }
    if (s_ipLabel) {
        std::string ip = currentIpText();
        if (ip != s_ipCache) {
            s_ipCache = ip;
            lv_label_set_text(s_ipLabel, ip.c_str());
        }
    }
    lvgl_port_unlock();
}