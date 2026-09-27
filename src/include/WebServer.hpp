#pragma once

#include <string>
#include "AppConfig.hpp"

// HTTP web UI + API server (port 80). Basic-auth protected.
// Firmware update: "restart in recovery mode" reboots into the safeboot stub,
// which runs a Tasmota-compatible /u2 + /u3 recovery web UI (direct-to-app0).
class WebServer {
public:
    static void start();
    static void stop();
    static void onConfigChanged(); // re-apply active source + dashboard
};