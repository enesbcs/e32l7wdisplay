#pragma once

#include <string>
#include "AppConfig.hpp"

// Shelly MQTT (Gen1 "shellies/" standard topics + Gen2 shellies_discovery)
class ShellyMQTT {
public:
    // external message callback for topics this module does not consume
    // (integration sharing); binary-safe (ptr,len), called on the MQTT task
    using ExtMsgCb = void (*)(const char *topic, const char *payload, int len);

    static void start(const AppConfig &config);
    static void stop();
    static bool isConnected();
    // manual re-discovery: broadcast triggers (no-op when offline)
    static void discover();

    // connection sharing for the HA integration: true when this client runs
    // with exactly these credentials (compare before borrowing the handle)
    static bool matches(const std::string &host, int port,
                        const std::string &user, const std::string &pass);
    static uint32_t connCount(); // increments per MQTT connect
    // publish/subscribe through the shared handle (<0 when unusable)
    static int publishShared(const char *topic, const char *payload,
                             int qos, bool retain);
    static int subscribeShared(const char *topic);
    static void setExtHandler(ExtMsgCb cb);
};