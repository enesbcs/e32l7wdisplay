#include "WiFiManager.hpp"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"
#include "LogBuffer.hpp"
#include "DnsServer.hpp"
#include <cstring>
#include <cstdio>


WiFiManager::Mode WiFiManager::s_mode = WiFiManager::Mode::CONNECTING;
void (*WiFiManager::s_configSavedCb)() = nullptr;
EventGroupHandle_t WiFiManager::s_wifi_event_group = nullptr;
std::string WiFiManager::s_lastSSID;

static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

void WiFiManager::begin() {
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
        if (sta_netif) {
            esp_netif_set_hostname(sta_netif, hostname);
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_NULL);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &eventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &eventHandler, nullptr);

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_wifi_start();

    LogBuffer::logf("WiFi manager initialized");
}

bool WiFiManager::connectSingle(const std::string &ssid, const std::string &password, bool dhcp,
                                const std::string &ip, const std::string &mask, const std::string &gw) {
    if (ssid.empty()) return false;
    s_mode = Mode::CONNECTING;
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) return false;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        // static IP applied only with four valid octets each and a non-zero
        // gateway; anything else falls back to DHCP instead of programming
        // garbage into the interface (uninitialized ipInfo before this fix)
        esp_netif_ip_info_t ipInfo = {};
        bool staticOk = !dhcp && !ip.empty() && !mask.empty() && !gw.empty()
            && esp_netif_str_to_ip4(ip.c_str(), &ipInfo.ip) == ESP_OK
            && esp_netif_str_to_ip4(mask.c_str(), &ipInfo.netmask) == ESP_OK
            && esp_netif_str_to_ip4(gw.c_str(), &ipInfo.gw) == ESP_OK
            && ipInfo.ip.addr != 0 && ipInfo.gw.addr != 0;
        if (staticOk) {
            esp_netif_dhcpc_stop(netif);
            esp_netif_set_ip_info(netif, &ipInfo);
        } else {
            if (!dhcp) LogBuffer::logf("static IP invalid, using DHCP");
            esp_netif_dhcpc_start(netif);
        }
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    if (esp_wifi_connect() != ESP_OK) return false;

    LogBuffer::logf("Connecting to SSID: %s", ssid.c_str());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        LogBuffer::logf("Connected to AP: %s", ssid.c_str());
        s_mode = Mode::CLIENT;
        s_lastSSID = ssid;
        return true;
    }
    LogBuffer::logf("Failed to connect to AP: %s", ssid.c_str());
    return false;
}

bool WiFiManager::connectWithConfig(const AppConfig &config) {
    bool ok = connectSingle(config.wifi_ssid, config.wifi_password,
                            config.use_dhcp, config.static_ip, config.static_netmask, config.static_gateway);
    if (!ok && !config.wifi_ssid2.empty()) {
        LogBuffer::logf("SSID1 failed, trying SSID2: %s", config.wifi_ssid2.c_str());
        ok = connectSingle(config.wifi_ssid2, config.wifi_password2,
                           config.use_dhcp, config.static_ip, config.static_netmask, config.static_gateway);
    }
    if (!ok) s_mode = Mode::CONNECTING;
    return ok;
}

void WiFiManager::startHotspot() {
    s_mode = Mode::HOTSPOT;

    std::string apName = getAPName();

    // Set fixed AP IP (iPhone captive portal compatibility)
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = ESP_IP4TOADDR(172, 218, 28, 1);
        ip_info.gw.addr = ESP_IP4TOADDR(172, 218, 28, 1);
        ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);
    }

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, apName.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = apName.length();
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.channel = 6;
    ap_config.ap.beacon_interval = 100;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        LogBuffer::logf("Failed to set AP config: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        LogBuffer::logf("Failed to start AP: %s", esp_err_to_name(err));
        return;
    }

    dns_captive_start();
    LogBuffer::logf("Hotspot started: SSID='%s', IP=172.218.28.1", apName.c_str());
}

std::string WiFiManager::getAPName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char apName[32];
    snprintf(apName, sizeof(apName), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return std::string(apName);
}

std::string WiFiManager::getMAC() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(macStr);
}

std::string WiFiManager::getIP() {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return "0.0.0.0";
    esp_netif_ip_info_t ipInfo;
    if (esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) return "0.0.0.0";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ipInfo.ip));
    return std::string(buf);
}

void WiFiManager::eventHandler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                LogBuffer::logf("WiFi STA started");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                LogBuffer::logf("WiFi connected");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                LogBuffer::logf("WiFi disconnected");
                if (s_mode == Mode::CONNECTING) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            }
            case WIFI_EVENT_AP_START:
                LogBuffer::logf("AP started");
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                LogBuffer::logf("Station connected to AP");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        LogBuffer::logf("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}