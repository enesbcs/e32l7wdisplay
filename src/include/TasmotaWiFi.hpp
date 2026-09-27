#pragma once

#include <string>

// Read WiFi credentials from Tasmota's NVS "Settings" blob (READ-ONLY).
//
// Tasmota settings layout (ESP32, Settings struct, see tasmota_types.h):
//   offset 0x000 : cfg_holder (uint16 == 4617)  - validity check
//   offset 0x017 : text_pool start (char pool of null-terminated strings)
// String indices inside the pool (SettingsTextIndex order, tasmota.h):
//   0=SET_OTAURL 1..3=MQTT prefixes 4=SSID1 5=SSID2 6=PWD1 7=PWD2
// We NEVER write to the blob; the display has its own config otherwise.
class TasmotaWiFi {
public:
    static bool readCredentials(std::string &ssid1, std::string &pwd1,
                                std::string &ssid2, std::string &pwd2);
};