#include "Integration.hpp"
#include "AppConfig.hpp"
#include "Dashboard.hpp"
#include "ShellyMQTT.hpp"
#include "WiFiManager.hpp"
#include "LogBuffer.hpp"
#include "Version.hpp"

#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cctype>
#include <string>

// HA MQTT integration (autodiscovery). Transport sharing: the Shelly client
// is borrowed when credentials match an active Shelly source, else a private
// client runs. Own-client strings are function-statics: the mqtt config only
// borrows the pointers (same lifetime class as the audit's WS/MQTT note).
// NOTE: only the 4 connection fields are kept, not a whole AppConfig copy
// (saves ~1KB internal RAM; AppConfig carries all URLs/tokens as std::string).
struct IntCreds {
    std::string host;
    int port = 1883;
    std::string user;
    std::string pass;
};
static IntCreds s_icfg;
static bool s_enabled = false;

enum class Transp { OFF, SHARED, OWN };
static Transp s_mode = Transp::OFF;

static esp_mqtt_client_handle_t s_own = nullptr;
static bool s_ownConnected = false;
static uint32_t s_lastShellyConn = 0;
static uint32_t s_lastSensMs = 0;

static std::string prefix() { return WiFiManager::getAPName(); }

static void pub(const std::string &topic, const std::string &payload, bool retain) {
    int rc = -1;
    if (s_mode == Transp::SHARED) {
        rc = ShellyMQTT::publishShared(topic.c_str(), payload.c_str(), 1, retain);
    } else if (s_mode == Transp::OWN && s_own && s_ownConnected) {
        rc = esp_mqtt_client_publish(s_own, topic.c_str(), payload.c_str(),
                                     0, 1, retain ? 1 : 0);
    } else {
        return;
    }
    if (rc < 0) LogBuffer::logf("integration pub failed: %s", topic.c_str());
}

static void sub(const std::string &topic) {
    if (s_mode == Transp::SHARED) {
        ShellyMQTT::subscribeShared(topic.c_str());
    } else if (s_mode == Transp::OWN && s_own && s_ownConnected) {
        esp_mqtt_client_subscribe(s_own, topic.c_str(), 1);
    }
}

static cJSON *devObj() {
    std::string mac = WiFiManager::getMAC();
    std::string id;
    for (char c : mac) {
        if (c != ':') id += (char)tolower((unsigned char)c);
    }
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(d, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(id.c_str()));
    cJSON_AddStringToObject(d, "mf", "L7");
    cJSON_AddStringToObject(d, "mdl", "e32l7wdisplay");
    cJSON_AddStringToObject(d, "name", prefix().c_str());
    cJSON_AddStringToObject(d, "sw", APP_FW_VERSION);
    return d;
}

static void addAvail(cJSON *o, const std::string &P) {
    cJSON_AddStringToObject(o, "avty_t", (P + "/online").c_str());
    cJSON_AddStringToObject(o, "pl_avail", "online");
    cJSON_AddStringToObject(o, "pl_not_avail", "offline");
}

static void publishDiscovery() {
    std::string P = prefix();
    // display on/off switch
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "L7 Display");
        cJSON_AddStringToObject(o, "uniq_id", (P + "-display").c_str());
        cJSON_AddStringToObject(o, "obj_id", (P + "_display").c_str());
        cJSON_AddStringToObject(o, "stat_t", (P + "/display/state").c_str());
        cJSON_AddStringToObject(o, "cmd_t", (P + "/display/set").c_str());
        cJSON_AddStringToObject(o, "pl_on", "ON");
        cJSON_AddStringToObject(o, "pl_off", "OFF");
        cJSON_AddStringToObject(o, "stat_on", "ON");
        cJSON_AddStringToObject(o, "stat_off", "OFF");
        cJSON_AddStringToObject(o, "icon", "mdi:monitor");
        cJSON_AddNumberToObject(o, "qos", 1);
        cJSON_AddItemToObject(o, "dev", devObj());
        addAvail(o, P);
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (s) {
            pub("homeassistant/switch/" + P + "/display/config", s, true);
            cJSON_free(s);
        }
    }
    // uptime sensor (duration renders Xd Ym in HA)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "L7 Uptime");
        cJSON_AddStringToObject(o, "uniq_id", (P + "-uptime").c_str());
        cJSON_AddStringToObject(o, "stat_t", (P + "/sensor/uptime").c_str());
        cJSON_AddStringToObject(o, "dev_cla", "duration");
        cJSON_AddStringToObject(o, "unit_of_measurement", "s");
        cJSON_AddStringToObject(o, "stat_cla", "measurement");
        cJSON_AddStringToObject(o, "entity_category", "diagnostic");
        cJSON_AddNumberToObject(o, "qos", 1);
        cJSON_AddItemToObject(o, "dev", devObj());
        addAvail(o, P);
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (s) {
            pub("homeassistant/sensor/" + P + "/uptime/config", s, true);
            cJSON_free(s);
        }
    }
    // MCU temperature sensor
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "L7 MCU Temp");
        cJSON_AddStringToObject(o, "uniq_id", (P + "-mcu-temp").c_str());
        cJSON_AddStringToObject(o, "stat_t", (P + "/sensor/mcu_temp").c_str());
        cJSON_AddStringToObject(o, "dev_cla", "temperature");
        // bare "C" is rejected by HA (valid: degree-C/F only)
        cJSON_AddStringToObject(o, "unit_of_measurement", "\xC2\xB0""C");
        cJSON_AddStringToObject(o, "stat_cla", "measurement");
        cJSON_AddStringToObject(o, "entity_category", "diagnostic");
        cJSON_AddNumberToObject(o, "qos", 1);
        cJSON_AddItemToObject(o, "dev", devObj());
        addAvail(o, P);
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (s) {
            pub("homeassistant/sensor/" + P + "/mcu_temp/config", s, true);
            cJSON_free(s);
        }
    }
}

void Integration::publishDisplayState() {
    pub(prefix() + "/display/state", Dashboard::isDisplayOn() ? "ON" : "OFF", true);
}

static void publishSensors() {
    std::string P = prefix();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    pub(P + "/sensor/uptime", std::to_string(up), false);
    float t = 0;
    if (Dashboard::readMcuTempC(t)) {
        char b[16];
        snprintf(b, sizeof(b), "%.1f", (double)t);
        pub(P + "/sensor/mcu_temp", b, false);
    }
}

static void publishAll() {
    publishDiscovery();
    pub(prefix() + "/online", "online", true);
    Integration::publishDisplayState();
    publishSensors();
}

static void onCommand(const std::string &payload) {
    std::string p;
    for (char c : payload) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            p += (char)tolower((unsigned char)c);
    }
    bool on;
    if (p == "on" || p == "1" || p == "true") on = true;
    else if (p == "off" || p == "0" || p == "false") on = false;
    else {
        LogBuffer::logf("integration: bad display command");
        return;
    }
    Dashboard::blankDisplay(!on);
    LogBuffer::logf("integration: display %s", on ? "ON" : "OFF");
    Integration::publishDisplayState();
}

static void onSharedMessage(const char *topic, const char *payload, int len) {
    if (!s_enabled || s_mode != Transp::SHARED || !topic || !payload || len <= 0)
        return;
    if (prefix() + "/display/set" == topic)
        onCommand(std::string(payload, (size_t)len));
}

static void ownHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    auto *event = (esp_mqtt_event_handle_t)data;
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            s_ownConnected = true;
            LogBuffer::logf("integration MQTT connected");
            sub(prefix() + "/display/set");
            publishAll();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_ownConnected = false;
            break;
        case MQTT_EVENT_DATA: {
            if (!event->topic || !event->data) break;
            std::string topic(event->topic, event->topic_len);
            if (topic == prefix() + "/display/set") {
                onCommand(std::string(event->data, event->data_len));
            }
            break;
        }
        default:
            break;
    }
}

static void stopOwn() {
    if (s_own) {
        esp_mqtt_client_stop(s_own);
        esp_mqtt_client_destroy(s_own);
        s_own = nullptr;
        s_ownConnected = false;
    }
}

static void startOwn() {
    stopOwn();
    if (s_icfg.host.empty()) return;
    // function-statics: the mqtt config borrows these pointers
    static std::string uri, user, pass, lwtTopic, lwtMsg;
    char ubuf[192];
    snprintf(ubuf, sizeof(ubuf), "mqtt://%s:%d", s_icfg.host.c_str(),
             s_icfg.port);
    uri = ubuf;
    user = s_icfg.user;
    pass = s_icfg.pass;
    lwtTopic = prefix() + "/online";
    lwtMsg = "offline";
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri.c_str();
    cfg.broker.address.port = s_icfg.port;
    // RX buffer halved (512B): our only inbound is the tiny display/set
    // command; TX stays 1024B for the ~900B discovery docs. Saves 512B
    // internal (both buffers are plain-malloced DRAM in esp-mqtt).
    cfg.buffer.size = 512;
    cfg.buffer.out_size = 1024;
    if (!user.empty()) cfg.credentials.username = user.c_str();
    if (!pass.empty()) cfg.credentials.authentication.password = pass.c_str();
    cfg.session.last_will.topic = lwtTopic.c_str();
    cfg.session.last_will.msg = lwtMsg.c_str();
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = true;
    cfg.session.keepalive = 60;
    cfg.network.reconnect_timeout_ms = 10000;
    s_own = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_own, (esp_mqtt_event_id_t)MQTT_EVENT_ANY,
                                   ownHandler, nullptr);
    esp_mqtt_client_start(s_own);
    LogBuffer::logf("integration MQTT started: %s", ubuf);
}

static void shutdown() {
    if (s_mode != Transp::OFF) pub(prefix() + "/online", "offline", true);
    stopOwn();
    s_mode = Transp::OFF;
}

void Integration::apply(const AppConfig &config) {
    s_icfg.host = config.mqtt_int_host;
    s_icfg.port = config.mqtt_int_port;
    s_icfg.user = config.mqtt_int_username;
    s_icfg.pass = config.mqtt_int_password;
    s_enabled = !config.mqtt_int_host.empty();
    ShellyMQTT::setExtHandler(onSharedMessage);
    if (!s_enabled) {
        shutdown();
        return;
    }
    bool wantShared = config.data_source == (int)DataSourceType::SHELLY
        && ShellyMQTT::matches(config.mqtt_int_host, config.mqtt_int_port,
                               config.mqtt_int_username,
                               config.mqtt_int_password);
    Transp want = wantShared ? Transp::SHARED : Transp::OWN;
    if (want != s_mode) {
        if (s_mode == Transp::OWN) stopOwn();
        s_mode = want;
        s_lastShellyConn = ShellyMQTT::connCount();
        if (s_mode == Transp::OWN) startOwn();
    } else if (s_mode == Transp::OWN && !s_own) {
        startOwn();
    }
    // best-effort immediate announce (tick + connect bursts repeat it)
    if (s_mode == Transp::SHARED) {
        sub(prefix() + "/display/set");
        publishAll();
    }
}

void Integration::tick(uint32_t nowMs) {
    static uint32_t last = 0;
    if (nowMs - last < 5000) return;
    last = nowMs;
    if (!s_enabled || s_mode == Transp::OFF) return;
    if (s_mode == Transp::SHARED) {
        // shelly (re)connects we cannot see otherwise: re-subscribe + announce
        uint32_t cc = ShellyMQTT::connCount();
        if (cc != s_lastShellyConn) {
            s_lastShellyConn = cc;
            sub(prefix() + "/display/set");
            publishAll();
        }
    }
    if (nowMs - s_lastSensMs >= 60000) {
        s_lastSensMs = nowMs;
        publishSensors();
    }
}
