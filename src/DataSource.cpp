#include "DataSource.hpp"
#include "AppConfig.hpp"
#include "BLEScan.hpp"
#include "HAWebSocket.hpp"
#include "ShellyMQTT.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int s_activeSource = -1; // unknown until first apply (boot)

void DataSource::stopAll() {
    HAWebSocket::stop();
    ShellyMQTT::stop();
    BLEScan::end();
}

void DataSource::apply(AppConfig &config) {
    if (s_activeSource != -1 && s_activeSource != config.data_source) {
        // genuine switch: pinned cells reference dead sensors from the old
        // source, drop them (and their name overrides) everywhere.
        config.cells.clear();
        config.name_override_ids.clear();
        ConfigManager::save(config);
        SensorRegistry::pruneStale(0); // drop all stale registry entries
        LogBuffer::logf("Data source switched: dashboard cells cleared");
    }
    s_activeSource = config.data_source;
    stopAll();
    switch ((DataSourceType)config.data_source) {
        case DataSourceType::BLE:
            BLEScan::begin();
            LogBuffer::logf("Data source: passive BLE");
            break;
        case DataSourceType::HOMEASSISTANT:
            HAWebSocket::start(config);
            LogBuffer::logf("Data source: Home Assistant WS");
            break;
        case DataSourceType::SHELLY:
            ShellyMQTT::start(config);
            LogBuffer::logf("Data source: Shelly MQTT");
            break;
        case DataSourceType::OFF:
            LogBuffer::logf("Data source: OFF (all sources stopped)");
            break;
        default:
            LogBuffer::logf("Unknown data source %d", config.data_source);
            break;
    }
}

bool DataSource::anySourceConnected(bool) {
    // notify on change not needed; connection state read from each source
    return HAWebSocket::isConnected() || ShellyMQTT::isConnected();
}