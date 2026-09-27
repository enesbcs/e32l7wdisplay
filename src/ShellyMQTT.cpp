#include "ShellyMQTT.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"

#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static esp_mqtt_client_handle_t s_mqtt = nullptr;
static bool s_connected = false;
// active credentials (for matches()) + connect generation + ext handler
static std::string s_host, s_user, s_pass;
static int s_port = 0;
static uint32_t s_connCount = 0;
static ShellyMQTT::ExtMsgCb s_extCb = nullptr;

bool ShellyMQTT::matches(const std::string &host, int port,
                         const std::string &user, const std::string &pass) {
    return s_mqtt && s_host == host && s_port == port
        && s_user == user && s_pass == pass;
}

uint32_t ShellyMQTT::connCount() { return s_connCount; }

int ShellyMQTT::publishShared(const char *topic, const char *payload,
                              int qos, bool retain) {
    if (!s_mqtt || !s_connected || !topic || !payload) return -1;
    return esp_mqtt_client_publish(s_mqtt, topic, payload, 0, qos,
                                   retain ? 1 : 0);
}

int ShellyMQTT::subscribeShared(const char *topic) {
    if (!s_mqtt || !s_connected || !topic) return -1;
    return esp_mqtt_client_subscribe(s_mqtt, topic, 1);
}

void ShellyMQTT::setExtHandler(ExtMsgCb cb) { s_extCb = cb; }
// diagnostics: first data messages are logged verbosely, then only a
// heartbeat (silent success made discovery failures undebuggable)
static uint32_t s_msgCount = 0;

static const char *TOPIC_ANNOUNCE_GEN1 = "shellies/announce";          // Gen1 device type-only announces
static const char *TOPIC_ANNOUNCE_GEN2 = "shellies_discovery/announce"; // Gen2 discovery
static const char *TOPIC_RPC = "shellies_discovery/rpc";                // Gen2 subscribe/command channel
static const char *TOPIC_RPC_ANY = "+/rpc";                             // Gen2 legacy single-level status
static const char *TOPIC_GEN2_EVENTS = "+/events/rpc";                  // Gen2/Gen3 NotifyStatus
static const char *TOPIC_GEN1_SENS = "shellies/+/sensor/+";             // Gen1 temp/hum/battery
static const char *TOPIC_GEN1_INFO = "shellies/+/info";                  // Gen1 wifi_sta.rssi
static const char *TOPIC_OURS = "e32l7wdisplay/rpc";                     // our GetConfig inbox
static const char *TOPIC_GEN1_ONLINE = "shellies/+/online";
static const char *TOPIC_ONLINE_ANY = "+/online";

static void shellyUpsert(const std::string &id, const char *kind, float value) {
    std::string sid = "shelly." + id;
    if (strcmp(kind, "temperature") == 0)
        SensorRegistry::upsert(sid, true, value, false, 0, false, 0);
    else if (strcmp(kind, "humidity") == 0)
        SensorRegistry::upsert(sid, false, 0, true, value, false, 0);
    else if (strcmp(kind, "battery") == 0)
        SensorRegistry::upsert(sid, false, 0, false, 0, true, value);
    else
        return;
    // row name: topic id without the "shelly." prefix (ID column keeps it);
    // temperature wins, others only fill gaps (HA parity)
    if (strcmp(kind, "temperature") == 0)
        SensorRegistry::setAutoName(sid, id);
    else
        SensorRegistry::setAutoName(sid, id, true);
    s_msgCount++;
    if (s_msgCount <= 10) {
        LogBuffer::logfSerial("Shelly data %s %s=%s", sid.c_str(), kind,
            std::to_string(value).c_str());
    } else if (s_msgCount % 50 == 0) {
        LogBuffer::logfSerial("Shelly MQTT: %u msgs seen", (unsigned)s_msgCount);
    }
}

// Gen1 numeric sensor data, validate ranges per HA discovery script
// topic format: shellies/<id>/sensor/<type>
static void handleGen1Sensor(const char *topic, const char *payload, int len) {
    const char *prefix = "shellies/";
    const char *p = strstr(topic, prefix);
    if (!p) return;
    const char *start = p + strlen(prefix);
    const char *slash1 = strchr(start, '/');
    if (!slash1) return;
    std::string id(start, slash1 - start);

    const char *kindStart = strstr(topic, "/sensor/");
    if (!kindStart) return;
    const char *kind = kindStart + strlen("/sensor/");

    char *end;
    float v = strtof(payload, &end);
    if (end == payload) return;

    if (strcmp(kind, "temperature") == 0) {
        if (v > -100 && v < 900) shellyUpsert(id, "temperature", v);
    } else if (strcmp(kind, "humidity") == 0) {
        if (v > 0 && v < 999) shellyUpsert(id, "humidity", v);
    } else if (strcmp(kind, "battery") == 0) {
        shellyUpsert(id, "battery", v);
    }
}

// Active discovery (teacher pattern): a device reporting online=true gets a
// Shelly.GetConfig probe on <prefix>/rpc; Gen2 answers on <src>/rpc (= ours).
// Own src (not shellies_discovery) keeps HA/bieniu flows undisturbed.
static int s_rpcId = 0;

static void sendGetConfig(const std::string &prefix) {
    if (!s_mqtt || !s_connected || prefix.empty()) return;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"src\":\"e32l7wdisplay\",\"method\":\"Shelly.GetConfig\"}",
             ++s_rpcId);
    std::string topic = prefix + "/rpc";
    esp_mqtt_client_publish(s_mqtt, topic.c_str(), buf, 0, 1, 0);
    LogBuffer::logf("Shelly probe GetConfig: %s", topic.c_str());
}

// Gen1 announce (shellies/announce): identity only, Gen1 has no RPC channel
static void handleGen1Announce(const char *payload, int len) {
    cJSON *msg = cJSON_ParseWithLength(payload, len);
    if (!msg) return;
    const cJSON *id = cJSON_GetObjectItem(msg, "id");
    const cJSON *model = cJSON_GetObjectItem(msg, "model");
    const cJSON *mac = cJSON_GetObjectItem(msg, "mac");
    const cJSON *gen = cJSON_GetObjectItem(msg, "gen");
    if (cJSON_IsString(id) && id->valuestring) {
        LogBuffer::logf("Shelly Gen%d announce: %s %s %s",
            (cJSON_IsNumber(gen) && (int)gen->valuedouble >= 2) ? 2 : 1,
            id->valuestring,
            (cJSON_IsString(model) && model->valuestring) ? model->valuestring : "?",
            (cJSON_IsString(mac) && mac->valuestring) ? mac->valuestring : "?");
    }
    cJSON_Delete(msg);
}

// GetConfig response on our inbox: learn listening prefix + mac
static void handleOurRpc(const char *payload, int len) {
    cJSON *msg = cJSON_ParseWithLength(payload, len);
    if (!msg) return;
    const cJSON *src = cJSON_GetObjectItem(msg, "src");
    const cJSON *result = cJSON_GetObjectItem(msg, "result");
    const char *dev = (cJSON_IsString(src) && src->valuestring) ? src->valuestring : "?";
    if (cJSON_IsObject(result)) {
        const char *pfx = "";
        const char *mac = "";
        const cJSON *mq = cJSON_GetObjectItem(result, "mqtt");
        if (cJSON_IsObject(mq)) {
            const cJSON *tp = cJSON_GetObjectItem(mq, "topic_prefix");
            if (cJSON_IsString(tp) && tp->valuestring) pfx = tp->valuestring;
        }
        const cJSON *sys = cJSON_GetObjectItem(result, "sys");
        if (cJSON_IsObject(sys)) {
            const cJSON *d = cJSON_GetObjectItem(sys, "device");
            if (cJSON_IsObject(d)) {
                const cJSON *m = cJSON_GetObjectItem(d, "mac");
                if (cJSON_IsString(m) && m->valuestring) mac = m->valuestring;
            }
        }
        LogBuffer::logf("Shelly Gen2 confirmed: %s prefix=%s mac=%s", dev, pfx, mac);
    }
    cJSON_Delete(msg);
}

// Gen1 info payload: wifi_sta.rssi (dBm) -> device RSSI.
// topic format: shellies/<id>/info. Attaches to the existing device row
// (setRssi updates entries only, so it lands once sensor data created it).
static void handleGen1Info(const char *topic, const char *payload, int len) {
    const char *prefix = "shellies/";
    const char *suffix = "/info";
    size_t tlen = strlen(topic);
    size_t slen = strlen(suffix);
    if (strncmp(topic, prefix, strlen(prefix)) != 0) return;
    if (tlen <= strlen(prefix) + slen) return;
    if (strcmp(topic + tlen - slen, suffix) != 0) return;
    std::string id(topic + strlen(prefix), tlen - strlen(prefix) - slen);
    if (id.empty()) return;

    cJSON *msg = cJSON_ParseWithLength(payload, len);
    if (!msg) return;
    int rssi = 0;
    bool have = false;
    const cJSON *wifi = cJSON_GetObjectItem(msg, "wifi_sta");
    if (cJSON_IsObject(wifi)) {
        const cJSON *r = cJSON_GetObjectItem(wifi, "rssi");
        if (cJSON_IsNumber(r)) {
            rssi = (int)r->valuedouble;
            have = true;
        }
    }
    cJSON_Delete(msg);
    if (!have || rssi > 0 || rssi < -150) return;
    std::string sid = "shelly." + id;
    SensorRegistry::setRssi(sid, rssi);
}

static void handleGen2Rpc(const char *topic, const char *payload, int len) {
    cJSON *msg = cJSON_ParseWithLength(payload, len);
    if (!msg) return;
    cJSON *method = cJSON_GetObjectItem(msg, "method");
    if (!method || !cJSON_IsString(method) || strcmp(method->valuestring, "NotifyStatus") != 0) {
        cJSON_Delete(msg);
        return;
    }
    // device id is the topic prefix before first '/'
    // (NOTE: custom multi-level prefixes collapse to their first segment)
    const char *slash = strchr(topic, '/');
    std::string id = slash ? std::string(topic, slash - topic) : std::string(topic);

    cJSON *statusObj = cJSON_GetObjectItem(msg, "params");
    if (!statusObj) statusObj = msg;
    cJSON *status = cJSON_GetObjectItem(statusObj, "status");
    cJSON *root = status ? status : statusObj;

    // temperature + humidity first: they create the registry entry that
    // setRssi below attaches to
    cJSON *tObj = cJSON_GetObjectItem(root, "temperature:0");
    if (tObj) {
        cJSON *tC = cJSON_GetObjectItem(tObj, "tC");
        if (cJSON_IsNumber(tC)) shellyUpsert(id, "temperature", (float)tC->valuedouble);
    }
    cJSON *hObj = cJSON_GetObjectItem(root, "humidity:0");
    if (hObj) {
        cJSON *rh = cJSON_GetObjectItem(hObj, "rh");
        if (cJSON_IsNumber(rh)) shellyUpsert(id, "humidity", (float)rh->valuedouble);
    }
    cJSON *batt = cJSON_GetObjectItem(root, "battery");
    if (cJSON_IsNumber(batt)) {
        shellyUpsert(id, "battery", (float)batt->valuedouble);
    } else {
        // Gen2/Gen3 DevicePower component object
        cJSON *dp = cJSON_GetObjectItem(root, "devicepower:0");
        if (cJSON_IsObject(dp)) {
            cJSON *b = cJSON_GetObjectItem(dp, "battery");
            if (cJSON_IsObject(b)) {
                cJSON *pct = cJSON_GetObjectItem(b, "percent");
                if (cJSON_IsNumber(pct))
                    shellyUpsert(id, "battery", (float)pct->valuedouble);
            }
        }
    }
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        cJSON *rssi = cJSON_GetObjectItem(wifi, "rssi");
        if (cJSON_IsNumber(rssi)) {
            int dbm = (int)rssi->valuedouble;
            if (dbm <= 0 && dbm >= -150) {
                std::string sid = "shelly." + id;
                SensorRegistry::setRssi(sid, dbm);
            }
        }
    }

    cJSON_Delete(msg);
}

// Broadcast discovery triggers on shellies/command (the one broadcast both
// generations honor): Gen1 answers announce (+state on update), Gen2 answers
// announce-info and publishes full status on status_update.
static void publishDiscovery() {
    if (!s_mqtt || !s_connected) {
        LogBuffer::logf("Shelly discover: offline, skipped");
        return;
    }
    esp_mqtt_client_publish(s_mqtt, "shellies/command", "announce", 8, 1, 0);
    esp_mqtt_client_publish(s_mqtt, "shellies/command", "update", 6, 1, 0);
    esp_mqtt_client_publish(s_mqtt, "shellies/command", "status_update", 13, 1, 0);
    LogBuffer::logf("Shelly discover: announce+update broadcast");
}

void ShellyMQTT::discover() {
    publishDiscovery();
}

static void mqttEventHandler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data) {
    auto *event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch (event_id) {
        // discovery triggers (Gen1 announce/update + Gen2 status_update),
        // sent on every connect AND on manual "Start discovering"
        case MQTT_EVENT_CONNECTED: {
            s_connected = true;
            s_connCount++;
            LogBuffer::logf("MQTT connected");
            auto sub = [&](const char *t) {
                int m = esp_mqtt_client_subscribe(client, t, 1);
                if (m < 0) LogBuffer::logf("MQTT sub FAILED %s: %d", t, m);
            };
            sub(TOPIC_ANNOUNCE_GEN1);
            sub(TOPIC_ANNOUNCE_GEN2);
            sub(TOPIC_RPC);
            sub(TOPIC_RPC_ANY);
            sub(TOPIC_GEN2_EVENTS);
            sub(TOPIC_GEN1_SENS);
            sub(TOPIC_GEN1_INFO);
            sub(TOPIC_OURS);
            sub(TOPIC_GEN1_ONLINE);
            sub(TOPIC_ONLINE_ANY);
            publishDiscovery();
            break;
        }
        case MQTT_EVENT_SUBSCRIBED:
            LogBuffer::logf("MQTT subscribed ack msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            LogBuffer::logf("MQTT error");
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) {
                s_connected = false;
                LogBuffer::logf("MQTT disconnected");
            }
            break;
        case MQTT_EVENT_DATA: {
            if (!event->topic || !event->data) break;
            std::string topic(event->topic, event->topic_len);
            std::string payload(event->data, event->data_len);

            if (topic == TOPIC_OURS) {
                handleOurRpc(payload.c_str(), payload.size());
            } else if (topic == TOPIC_ANNOUNCE_GEN1) {
                handleGen1Announce(payload.c_str(), payload.size());
            } else if (topic.size() > 7 &&
                       topic.compare(topic.size() - 7, 7, "/online") == 0 &&
                       payload == "true") {
                // teacher pattern: online device identifies itself on probe
                sendGetConfig(topic.substr(0, topic.size() - 7));
            } else if (topic == TOPIC_ANNOUNCE_GEN2) {
                // Gen2 discovery announce; HA integration sends GetConfig on subscribe -> nothing needed here
                cJSON *msg = cJSON_Parse(payload.c_str());
                if (msg) {
                    cJSON *idObj = cJSON_GetObjectItem(msg, "id");
                    if (cJSON_IsString(idObj))
                        LogBuffer::logf("Gen2 device announce: %s", idObj->valuestring);
                    cJSON_Delete(msg);
                }
            // NOTE: compare(0, 9, ...) - the old compare(0, 15, ...) never
            // matched (15-char slice vs 9-char literal differ in length),
            // so Gen1 sensor ingestion was silently dead until now.
            } else if (topic.compare(0, 9, "shellies/") == 0 && topic.find("/sensor/") != std::string::npos) {
                handleGen1Sensor(topic.c_str(), payload.c_str(), payload.size());
            } else if (topic.compare(0, 9, "shellies/") == 0 && topic.size() > 14 &&
                       topic.compare(topic.size() - 5, 5, "/info") == 0) {
                handleGen1Info(topic.c_str(), payload.c_str(), payload.size());
            } else if (topic.size() > 4 && topic.substr(topic.size() - 4) == "/rpc") {
                handleGen2Rpc(topic.c_str(), payload.c_str(), payload.size());
            } else if (s_extCb) {
                s_extCb(topic.c_str(), payload.c_str(), (int)payload.size());
            }
            break;
        }
        default:
            break;
    }
}

void ShellyMQTT::start(const AppConfig &config) {
    stop();
    if (config.mqtt_host.empty()) {
        LogBuffer::logf("MQTT disabled (no host)");
        return;
    }
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", config.mqtt_host.c_str(), config.mqtt_port);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    cfg.broker.address.port = config.mqtt_port;
    if (!config.mqtt_username.empty()) cfg.credentials.username = config.mqtt_username.c_str();
    if (!config.mqtt_password.empty()) cfg.credentials.authentication.password = config.mqtt_password.c_str();
    cfg.session.keepalive = 60;
    cfg.network.reconnect_timeout_ms = 10000;

    s_host = config.mqtt_host;
    s_port = config.mqtt_port;
    s_user = config.mqtt_username;
    s_pass = config.mqtt_password;
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, (esp_mqtt_event_id_t)MQTT_EVENT_ANY, mqttEventHandler, nullptr);
    esp_mqtt_client_start(s_mqtt);
    LogBuffer::logf("MQTT client started: %s", uri);
}

void ShellyMQTT::stop() {
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = nullptr;
        s_connected = false;
    }
    s_host.clear();
    s_user.clear();
    s_pass.clear();
    s_port = 0;
    s_msgCount = 0;
}

bool ShellyMQTT::isConnected() { return s_connected; }