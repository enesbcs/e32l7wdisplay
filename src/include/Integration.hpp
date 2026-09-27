#pragma once

#include <cstdint>
#include "AppConfig.hpp"

// Home Assistant MQTT integration (autodiscovery): display on/off switch,
// uptime + MCU temperature sensors, LWT availability. Topic prefix is the
// AP name (esp32-xxxxxx), never stored. Transport is shared with the Shelly
// client when host/port/user/pass all match AND the Shelly source is active,
// otherwise a dedicated client runs. All transitions are idempotent
// (retained discovery/state makes repeats harmless).
class Integration {
public:
    static void apply(const AppConfig &config);
    // called ~1s from the main loop (migration + 60s sensor refresh)
    static void tick(uint32_t nowMs);
    // republish the retained display state (call after out-of-band changes)
    static void publishDisplayState();
};
