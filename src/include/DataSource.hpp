#pragma once

#include <string>
#include "AppConfig.hpp"

// Activates the configured data source (BLE / Home Assistant / Shelly / OFF).
// On a real source SWITCH the dashboard cells are cleared (old-source sensor
// ids would never update again), the config is re-saved and the caller must
// REBOOT (returns true): the BLE stack has no teardown path (end() only
// cancels discovery, controller + NimBLE host stay resident), so a live
// BLE->anything switch OOMs internal RAM. Skipping the live re-apply also
// avoids crashing before the reboot. Re-applying the same source (boot,
// reconnect, plain save) keeps the cells and returns false.
class DataSource {
public:
    static bool apply(AppConfig &config);
    static void stopAll();
    static bool anySourceConnected(bool force = false);
};