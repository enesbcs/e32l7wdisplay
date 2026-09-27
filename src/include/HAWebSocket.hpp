#pragma once

#include <string>
#include "AppConfig.hpp"

// Home Assistant WebSocket client (plain ws://, LAN only)
// protocol: https://developers.home-assistant.io/docs/api/websocket
class HAWebSocket {
public:
    static void start(const AppConfig &config);
    static void stop();
    static bool isConnected();
    // auth watchdog: call ~1s from the main loop; resends a silent auth,
    // reconnects the client as a last resort
    static void tick(uint32_t nowMs);
    // dashboard assignment filter for live events (setup phase = empty)
    static void setWatchedCells(const std::vector<std::string> &cells);
};