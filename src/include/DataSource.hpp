#pragma once

#include <string>
#include "AppConfig.hpp"

// Activates the configured data source (BLE / Home Assistant / Shelly / OFF).
// On a real source SWITCH the dashboard cells are cleared (old-source sensor
// ids would never update again) and the config is re-saved. Re-applying the
// same source (boot, reconnect, plain save) keeps the cells.
class DataSource {
public:
    static void apply(AppConfig &config);
    static void stopAll();
    static bool anySourceConnected(bool force = false);
};