#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "AppConfig.hpp"

class WiFiManager {
public:
    enum class Mode {
        HOTSPOT,
        CLIENT,
        CONNECTING
    };

    static void begin();
    static bool connectWithConfig(const AppConfig &config);
    static void startHotspot();
    static Mode getMode() { return s_mode; }
    static std::string getAPName();
    static std::string getMAC();
    static std::string getIP();

    static void setConfigSavedCallback(void (*cb)()) { s_configSavedCb = cb; }
    static void onConfigSaved() { if (s_configSavedCb) s_configSavedCb(); }

    static std::string s_lastSSID;

private:
    static Mode s_mode;
    static void (*s_configSavedCb)();
    static EventGroupHandle_t s_wifi_event_group;

    static void eventHandler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
    static bool connectSingle(const std::string &ssid, const std::string &password, bool dhcp,
                              const std::string &ip, const std::string &mask, const std::string &gw);
};