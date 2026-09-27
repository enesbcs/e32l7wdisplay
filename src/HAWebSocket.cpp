#include "HAWebSocket.hpp"
#include "Sensor.hpp"
#include "LogBuffer.hpp"

#include "esp_websocket_client.h"
#include "esp_event.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <map>
#include <new>
#include <utility>
#include "esp_timer.h"

// transparent less<>: heterogeneous find(const char*) without key copy
using EntityMap = std::map<PsramString, PsramString, std::less<>,
                            PsramAllocator<std::pair<const PsramString, PsramString>>>;

static esp_websocket_client_handle_t s_client = nullptr;
static bool s_connected = false;
static bool s_authed = false;
static int s_msgId = 1;
static std::string s_token;
static PsramString s_frag;
// Per-chunk/event chatter (liveness, dribble, frame-complete, per-event RX)
// is pure serial noise in normal operation: errors and the registry/states
// summaries stay, the rest compiles out. Set to 1 for protocol debugging.
#define HA_WS_VERBOSE 0



// entity_id -> device_id (SPIRAM, built from the entity registry)
static EntityMap *s_entities = nullptr;
// Assignment filter: watched dashboard row ids ("ha:<device>"). Empty (setup
// phase, no cells assigned) means watch everything; otherwise live events for
// unassigned devices are dropped before upsert (registry/states bulks still
// run whole so discovery keeps working).
static PsramVector<PsramString> s_watched;
static bool s_watchAll = true;
// command ids so result frames can be routed (registry -> states -> subscribe)
static int s_idRegistry = 0;
static int s_idStates = 0;

static esp_timer_handle_t s_statesTimer = nullptr;
static bool s_registryDone = false;

static void wsSend(const char *fmt, ...);

// Backstop: if the registry answer never arrives, still fetch states
// (per-entity fallback) instead of waiting forever.
static void requestStates() {
    if (!s_client || s_idStates != 0) return;
    s_idStates = s_msgId++;
    wsSend("{\"id\":%d,\"type\":\"get_states\"}", s_idStates);
}

static bool s_statesPending = false;

static void statesTimerCb(void *arg) {
    (void)arg;
    if (!s_client || s_registryDone) return;
    // NOTE: no send from here - the client lock may be held by an ongoing
    // bulk read. Just raise the flag; the next DATA event (on the WS task,
    // recursive lock) performs the deferred send.
    LogBuffer::logfSerial("HA WS: registry timeout, states will follow next event");
    s_statesPending = true;
}

static void statesTimerStop() {
    if (s_statesTimer) {
        esp_timer_stop(s_statesTimer);
        esp_timer_delete(s_statesTimer);
        s_statesTimer = nullptr;
    }
}

static void statesTimerStart() {
    statesTimerStop();
    const esp_timer_create_args_t args = {
        .callback = statesTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ha_states",
    };
    if (esp_timer_create(&args, &s_statesTimer) == ESP_OK)
        esp_timer_start_once(s_statesTimer, 8000000);
}

// ---- streaming entity-registry parse ---------------------------------
// The registry answer (500KB and growing) is parsed element-by-element
// straight off the 1KB transport chunks: only sensor.* + device_id pairs
// are kept, everything else is discarded on the fly. Steady state stays
// under ~32KB PSRAM for ANY registry size (the 1MB accumulate-then-DOM
// path would drop the frame instead). Small messages keep the legacy path.
enum StreamPhase { SS_OFF, SS_SEEK_ARRAY, SS_IN_ARRAY };
static bool s_streamActive = false; // this inbound frame = registry result
static bool s_streamDead = false;   // pathological frame: drop silently
static StreamPhase s_ssPhase = StreamPhase::SS_OFF;
static PsramString s_tail;          // incomplete element bytes across chunks
static size_t s_scanned = 0;        // tail bytes already scanned (each byte is
                                    // scanned EXACTLY once: rescanning old bytes
                                    // with the new entry string-state inverts the
                                    // quote phase and silently drops elements)
static int s_arrDepth = 0;
static bool s_inStr = false;
static bool s_esc = false;
static int s_elStart = -1;          // tail-relative '{' of open element (-1 none)
static int s_elDepth = 0;           // brace depth of open element
static int s_regKept = 0;
static int s_regShown = 0;

static void streamReset() {
    s_streamActive = false;
    s_streamDead = false;
    s_ssPhase = StreamPhase::SS_OFF;
    s_tail.clear();
    s_scanned = 0;
    s_arrDepth = 0;
    s_inStr = false;
    s_esc = false;
    s_elStart = -1;
    s_elDepth = 0;
    s_regKept = 0;
    s_regShown = 0;
}

// raw integer value of "key" within the head bytes (envelope peek)
static bool rawIntVal(const char *data, int len, const char *key, int &out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    int n = len < 128 ? len : 128;
    for (int i = 0; i + (int)plen + 2 < n; i++) {
        if (memcmp(data + i, pat, plen) != 0) continue;
        int j = i + (int)plen;
        while (j < n && (data[j] == ' ' || data[j] == '\t')) j++;
        if (j >= n || data[j] != ':') continue;
        j++;
        while (j < n && (data[j] == ' ' || data[j] == '\t')) j++;
        if (j >= n) return false;
        bool neg = false;
        if (data[j] == '-') { neg = true; j++; }
        if (j >= n || data[j] < '0' || data[j] > '9') continue;
        int v = 0;
        while (j < n && data[j] >= '0' && data[j] <= '9')
            v = v * 10 + (data[j++] - '0');
        out = neg ? -v : v;
        return true;
    }
    return false;
}

static void processRegistryElement(const char *data, int len) {
    cJSON *el = cJSON_ParseWithLength(data, len);
    if (!el) return;
    const cJSON *eid = cJSON_GetObjectItem(el, "entity_id");
    const cJSON *did = cJSON_GetObjectItem(el, "device_id");
    if (!cJSON_IsString(eid) || !eid->valuestring
        || strncmp(eid->valuestring, "sensor.", 7) != 0) {
        cJSON_Delete(el);
        return;
    }
    s_regKept++; // same counting as the legacy DOM path (all sensor.*)
    if (cJSON_IsString(did) && did->valuestring && did->valuestring[0]) {
        (*s_entities)[eid->valuestring] = did->valuestring;
        if (s_regShown < 3) {
            LogBuffer::logfSerial("HA WS: e.g. %s", eid->valuestring);
            s_regShown++;
        }
    }
    cJSON_Delete(el);
}

static void finishRegistryStream() {
    LogBuffer::logfSerial("HA WS: registry: %d sensor entities", s_regKept);
    streamReset();
    s_registryDone = true;
    statesTimerStop();
    requestStates();
}

// feed one registry chunk; complete top-level {...} elements are parsed
// immediately, the incomplete tail stays buffered (bounded below).
// INVARIANT: every tail byte is scanned EXACTLY once, in order, with live
// element/string state (s_scanned/s_elStart/s_elDepth persist across calls).
// Re-scanning already-scanned bytes with the new entry string-state inverts
// the quote phase whenever a chunk ends mid-string and silently drops
// elements (fuzz seeds 101..106: last element lost, stalls, cascades).
static void streamRegistryChunk(const char *data, int len) {
    if (len <= 0 || !data) return;
    if (s_tail.size() + (size_t)len > (size_t)(64 * 1024)) {
        LogBuffer::logfSerial("HA WS: registry stream overrun, dropping frame");
        s_streamDead = true;
        return;
    }
    s_tail.append(data, len);
    const char *buf = s_tail.c_str();
    size_t blen = s_tail.size();
    size_t consumed = 0;
    if (s_ssPhase == StreamPhase::SS_SEEK_ARRAY) {
        // envelope: ..."result":[  (the "type":"result" value never matches:
        // a comma/brace follows it, never a colon+bracket)
        bool found = false;
        // 7-byte overlap: a split "\"result\"" (8B) refinds on the next chunk.
        // An undecided match (':'/'[' not arrived yet) rewinds s_scanned to
        // the match start: the next call re-examines it with the new bytes.
        // Without the rewind the pattern start falls out of the overlap
        // window and the array is never found (fuzz seed 400: empty [] lost).
        size_t from = (s_scanned >= 7) ? (s_scanned - 7) : 0;
        size_t cand = blen; // start of undecided match, blen = none
        for (size_t i = from; i + 8 <= blen; i++) {
            if (memcmp(buf + i, "\"result\"", 8) != 0) continue;
            size_t j = i + 8;
            while (j < blen && (buf[j] == ' ' || buf[j] == '\t')) j++;
            if (j >= blen) { cand = i; break; }
            if (buf[j] != ':') continue;
            j++;
            while (j < blen && (buf[j] == ' ' || buf[j] == '\t')) j++;
            if (j >= blen) { cand = i; break; } // split envelope: wait
            if (buf[j] != '[') continue;
            s_ssPhase = StreamPhase::SS_IN_ARRAY;
            s_arrDepth = 1;
            s_inStr = false;
            s_esc = false;
            s_elStart = -1;
            s_elDepth = 0;
            s_scanned = j + 1;
            consumed = j + 1;
            found = true;
            break;
        }
        if (!found) { s_scanned = cand; return; }
    }
    size_t i = s_scanned;
    bool finished = false; // array-close fired finishRegistryStream (state reset)
    for (; i < blen; i++) {
        char c = buf[i];
        if (s_inStr) {
            if (s_esc) s_esc = false;
            else if (c == '\\') s_esc = true;
            else if (c == '"') s_inStr = false;
            continue;
        }
        if (c == '"') { s_inStr = true; continue; }
        if (c == '[') { s_arrDepth++; continue; }
        if (c == ']') {
            s_arrDepth--;
            if (s_arrDepth == 0) {
                finishRegistryStream(); // logs + chains states, deactivates
                finished = true;        // tail/state already reset, skip below
                break;
            }
            continue;
        }
        if (c == '{') {
            if (s_arrDepth == 1 && s_elDepth == 0) s_elStart = (int)i;
            s_elDepth++;
            continue;
        }
        if (c == '}') {
            if (s_elDepth > 0) s_elDepth--;
            if (s_elDepth == 0 && s_arrDepth == 1 && s_elStart >= 0) {
                processRegistryElement(buf + s_elStart, (int)(i - (size_t)s_elStart + 1));
                s_elStart = -1;
                consumed = i + 1;
            }
            continue;
        }
    }
    if (finished) return; // streamReset already ran inside finish
    s_scanned = blen; // loop ran to end
    if (consumed > 0) {
        s_tail.erase(0, consumed);
        s_scanned -= consumed;
        if (s_elStart >= 0) {
            s_elStart -= (int)consumed; // open element always starts past consumed
            if (s_elStart < 0) { s_elStart = -1; s_elDepth = 0; } // safety net
        }
    }
}

// Auth watchdog. NOTE (1.4.41 crash fix): the main task has only a 4KB
// stack, so tick() must NEVER call into the client library (send/stop/start
// nest deeply enough to overflow it - observed as "stack overflow in task
// main" right after an in-tick retry). tick() only spawns this 8KB worker;
// all client calls happen here. A timer callback cannot do it either:
// the WS task may be silent and esp_timer context is wrong for stop/start.
// Policy: resend silent auth up to 4x (12s apart), then reconnect outright.
static uint32_t s_authSentMs = 0;
static bool s_authSent = false; // an auth frame already went out this attempt
static bool s_authWorkerRunning = false;

static void authWorker(void *arg) {
    (void)arg;
    for (int i = 1; i <= 4; i++) {
        if (!s_client || s_authed) break;
        LogBuffer::logfSerial("HA WS: auth retry #%d", i);
        if (!s_token.empty()) {
            s_authSent = true;
            wsSend("{\"type\":\"auth\",\"access_token\":\"%s\"}", s_token.c_str());
        }
        for (int k = 0; k < 12; k++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!s_client || s_authed) break;
        }
    }
    bool need = (s_client != nullptr && !s_authed);
    s_authWorkerRunning = false;
    if (need) {
        LogBuffer::logfSerial("HA WS: auth stalled, reconnecting");
        AppConfig live = getLiveConfig();
        HAWebSocket::stop();
        if (live.data_source == (int)DataSourceType::HOMEASSISTANT)
            HAWebSocket::start(live);
    }
    vTaskDelete(nullptr);
}

void HAWebSocket::tick(uint32_t nowMs) {
    if (!s_client || !s_connected || s_authed || s_authWorkerRunning) return;
    if (nowMs - s_authSentMs < 12000) return;
    s_authSentMs = nowMs;
    s_authWorkerRunning = true;
    if (xTaskCreate(authWorker, "ha_auth", 8192, nullptr, 5, nullptr) != pdPASS)
        s_authWorkerRunning = false;
}

static void wsSend(const char *fmt, ...) {
    if (!s_client) return;
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int rc = esp_websocket_client_send_text(s_client, buf, strlen(buf), 1000 / portTICK_PERIOD_MS);
    if (rc < 0)
        LogBuffer::logfSerial("HA WS: send failed: %d [%s]", rc, buf);
}

// One registry entry per physical device: entities sharing a device_id
// (e.g. sensor.kert_homerseklet/_paratartalom/_elem) merge into "ha:<device>".
// Entities without device_id fall back to "ha:<entity_id>".
static PsramString haDeviceFor(const char *entityId) {
    if (s_entities) {
        auto it = s_entities->find(entityId ? entityId : "");
        if (it != s_entities->end() && !it->second.empty()) return it->second;
    }
    return entityId ? entityId : "";
}

// Readable row name = entity_id with the uniform "sensor." prefix cut.
// Nothing more: registry/device names proved generic or duplicated
// ("System Monitor"), the entity_id is the exact reference.
static PsramString shortEntityName(const char *entityId) {
    const char *p = entityId ? entityId : "";
    if (strncmp(p, "sensor.", 7) == 0) p += 7;
    return PsramString(p);
}

static void haUpsert(const PsramString &entityId, const char *kind, float value) {
    PsramString dev = haDeviceFor(entityId.c_str());
    bool hasDevice = (dev != entityId);
    PsramString id = PsramString("ha:") + dev;
    bool hasT = false, hasH = false, hasB = false;
    float t = 0, h = 0, b = 0;
    if (strcmp(kind, "temperature") == 0) { hasT = true; t = value; }
    else if (strcmp(kind, "humidity") == 0) { hasH = true; h = value; }
    else if (strcmp(kind, "battery") == 0) { hasB = true; b = value; }
    SensorRegistry::upsert(id, hasT, t, hasH, h, hasB, b);
    if (strcmp(kind, "rssi") == 0) SensorRegistry::setRssi(id, (int)value);
    // grouping info for the Discovered list (one row per physical device)
    SensorRegistry::setDeviceMember(id, hasDevice ? dev : "", entityId);
    // row name: the temperature member's short entity (others only fill gaps)
    if (strcmp(kind, "temperature") == 0)
        SensorRegistry::setAutoName(id, shortEntityName(entityId.c_str()));
    else
        SensorRegistry::setAutoName(id, shortEntityName(entityId.c_str()), true);
}

// Validated entity_id accessor: non-string/missing/empty ids (malicious
// HA, corrupt frame) are skipped silently instead of crashing on a NULL
// valuestring dereference. Used at every network-JSON entry point.
static const char *entityIdOf(const cJSON *obj) {
    const cJSON *eid = cJSON_GetObjectItem(obj, "entity_id");
    if (!cJSON_IsString(eid) || !eid->valuestring || !eid->valuestring[0])
        return nullptr;
    return eid->valuestring;
}

// returns kind string matching, nullptr if not a sensor of interest
static const char *classifySensor(const cJSON *entityState, const cJSON *attributes) {
    const char *entityId = entityIdOf(entityState);
    if (!entityId) return nullptr;
    if (strncmp(entityId, "sensor.", 7) != 0) return nullptr;

    const cJSON *dc = cJSON_GetObjectItem(attributes, "device_class");
    if (cJSON_IsString(dc)) {
        if (strcmp(dc->valuestring, "temperature") == 0) return "temperature";
        if (strcmp(dc->valuestring, "humidity") == 0) return "humidity";
        if (strcmp(dc->valuestring, "battery") == 0) return "battery";
        if (strcmp(dc->valuestring, "signal_strength") == 0) return "rssi";
    }
    // no device_class: heuristic on the attribute names
    const cJSON *uatom = cJSON_GetObjectItem(attributes, "unit_of_measurement");
    if (cJSON_IsString(uatom)) {
        const char *u = uatom->valuestring;
        if (u[0] == '°' || (u[0] == 'C' && u[1] == '\0') ||
            strstr(u, "emperature") || strstr(u, "Celsius"))
            return "temperature";
        if (strchr(u, '%') && (strstr(u, "hum") || strstr(u, "rel"))) return "humidity";
    }
    // fallback heuristics on name suffix
    if ((strstr(entityId, "_temperature") || strstr(entityId, "_temp")) &&
        strstr(entityId, "humidity") == nullptr)
        return "temperature";
    if (strstr(entityId, "_humidity")) return "humidity";
    if (strstr(entityId, "_battery")) return "battery";
    if (strstr(entityId, "_rssi") || strstr(entityId, "signal_strength")
        || strstr(entityId, "_signal")) return "rssi";
    return nullptr;
}

static void parseStateList(const cJSON *states) {
    const cJSON *el;
    cJSON_ArrayForEach(el, states) {
        if (!entityIdOf(el)) continue;
        const cJSON *attr = cJSON_GetObjectItem(el, "attributes");
        if (!attr) continue;
        const char *kind = classifySensor(el, attr);
        if (!kind) continue;

        const cJSON *stateObj = cJSON_GetObjectItem(el, "state");
        if (!cJSON_IsString(stateObj)) continue;
        const char *stateStr = stateObj->valuestring;
        if (!stateStr[0]) continue;
        if (strcmp(stateStr, "unavailable") == 0 || strcmp(stateStr, "unknown") == 0) continue;

        char *end;
        float value = strtof(stateStr, &end);
        if (end == stateStr) continue; // not numeric
        const char *eid = entityIdOf(el);
        if (eid) haUpsert(eid, kind, value);
    }
}

static int s_evCount = 0;

// raw "key":"value" scan (tolerates spaces). Used to route messages without
// building a cJSON DOM first; any doubt returns safe defaults (parse).
static bool rawStringVal(const char *data, int len, const char *key, std::string &out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    int n = len < 128 ? len : 128;
    for (int i = 0; i + (int)plen + 2 < n; i++) {
        if (memcmp(data + i, pat, plen) != 0) continue;
        int j = i + (int)plen;
        while (j < n && (data[j] == ' ' || data[j] == '\t' || data[j] == '\n' || data[j] == '\r')) j++;
        if (j >= n || data[j] != ':') continue;
        j++;
        while (j < n && (data[j] == ' ' || data[j] == '\t' || data[j] == '\n' || data[j] == '\r')) j++;
        if (j >= n || data[j] != '"') continue;
        j++;
        out.clear();
        while (j < n && data[j] != '"') {
            if (data[j] == '\\') return false; // escaped: give up, parse fully
            out += data[j++];
        }
        if (j >= n) return false;
        return true;
    }
    return false;
}

// Pre-parse router: state_changed events for unwatched rows are dropped
// before the ~1.5KB cJSON DOM is built. Setup phase (watchAll) and every
// non-event message always parse (fail-open to current behavior).
static bool shouldParseMessage(const char *data, int len) {
    if (s_watchAll) return true;
    std::string t;
    if (!rawStringVal(data, len, "type", t) || t != "event") return true;
    std::string eid;
    if (!rawStringVal(data, len, "entity_id", eid)) return true;
    PsramString rowId = PsramString("ha:") + haDeviceFor(eid.c_str());
    for (auto &w : s_watched) {
        if (w == rowId) return true;
    }
    return false;
}

static void handleMessage(const char *data, int len) {
    if (!shouldParseMessage(data, len)) return;
    cJSON *msg = cJSON_ParseWithLength(data, len);
    if (!msg) {
        LogBuffer::logfSerial("HA WS: RX %dB unparsable", len);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(msg); return; }
    const char *t = type->valuestring;
    const cJSON *idObj = cJSON_GetObjectItem(msg, "id");
    int mid = (idObj && cJSON_IsNumber(idObj)) ? idObj->valueint : -1;
    // steady-state state_changed events are noise; handshake/results stay
    bool eventNoise = (strcmp(t, "event") == 0);
#if HA_WS_VERBOSE
    eventNoise = false;
#endif
    if (!eventNoise) {
        if (mid >= 0) LogBuffer::logfSerial("HA WS: RX %s %dB id=%d", t, len, mid);
        else LogBuffer::logfSerial("HA WS: RX %s %dB", t, len);
    }

    if (strcmp(t, "auth_required") == 0) {
        // auth carries no id: a duplicate (watchdog already sent one while
        // this was in flight) makes HA answer "Message incorrectly formatted"
        if (!s_authSent) {
            s_authSent = true;
            wsSend("{\"type\":\"auth\",\"access_token\":\"%s\"}", s_token.c_str());
            // token content NEVER logged; length tells truncation/doubling apart
            LogBuffer::logfSerial("HA WS: authenticating (token %u chars)", (unsigned)s_token.size());
        }
        s_authSentMs = (uint32_t)(esp_timer_get_time() / 1000);
    } else if (strcmp(t, "auth_ok") == 0) {
        s_authed = true;
        // fresh ids: a re-auth (retry/reconnect) must re-run the whole chain
        s_idStates = 0;
        s_statesPending = false;
        // Fire-and-forget order: live events flow at once; a slow/failed
        // bulk response can never stall the rest. get_states follows the
        // registry result (success or fail).
        int sub = s_msgId++;
        wsSend("{\"id\":%d,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}", sub);
        s_idRegistry = s_msgId++;
        wsSend("{\"id\":%d,\"type\":\"config/entity_registry/list\"}", s_idRegistry);
        s_registryDone = false;
        statesTimerStart();
        LogBuffer::logfSerial("HA WS: authenticated, subscribed + registry requested");
    } else if (strcmp(t, "auth_invalid") == 0) {
        LogBuffer::logfSerial("HA WS: auth invalid!");
    } else if (strcmp(t, "event") == 0) {
        const cJSON *ev = cJSON_GetObjectItem(msg, "event");
        const cJSON *evType = ev ? cJSON_GetObjectItem(ev, "event_type") : nullptr;
        if (cJSON_IsString(evType) && strcmp(evType->valuestring, "state_changed") == 0) {
            // state + attributes live in data.new_state (not in data itself!)
            const cJSON *dataObj = cJSON_GetObjectItem(ev, "data");
            const char *eidStr = (dataObj && cJSON_IsObject(dataObj))
                ? entityIdOf(dataObj) : nullptr;
            const cJSON *newState = (dataObj && cJSON_IsObject(dataObj))
                ? cJSON_GetObjectItem(dataObj, "new_state") : nullptr;
            const cJSON *attr = cJSON_IsObject(newState)
                ? cJSON_GetObjectItem(newState, "attributes") : nullptr;
            const char *kind = (eidStr && cJSON_IsObject(attr))
                ? classifySensor(newState, attr) : nullptr;
            const cJSON *stateObj = kind ? cJSON_GetObjectItem(newState, "state") : nullptr;
            if (cJSON_IsString(stateObj) && stateObj->valuestring[0]) {
                char *end;
                float value = strtof(stateObj->valuestring, &end);
                if (end != stateObj->valuestring) {
                    // assignment filter (normal phase): drop events that do
                    // not belong to a dashboard row before upsert
                    bool hit = s_watchAll;
                    if (!hit && eidStr) {
                        PsramString rowId = PsramString("ha:")
                            + haDeviceFor(eidStr);
                        for (auto &w : s_watched) {
                            if (w == rowId) { hit = true; break; }
                        }
                    }
                    if (hit && eidStr) {
                        haUpsert(eidStr, kind, value);
                        if (s_evCount < 3) {
                            LogBuffer::logfSerial("HA WS: event %s %s=%s", eidStr,
                                kind, stateObj->valuestring);
                        }
                        s_evCount++;
                        if (s_evCount % 50 == 0)
                            LogBuffer::logfSerial("HA WS: %d events seen", s_evCount);
                    }
                }
            }
        }
    } else if (strcmp(t, "result") == 0) {
        const cJSON *idObj = cJSON_GetObjectItem(msg, "id");
        int rid = (idObj && cJSON_IsNumber(idObj)) ? idObj->valueint : -1;
        const cJSON *success = cJSON_GetObjectItem(msg, "success");
        if (success && cJSON_IsFalse(success)) {
            const cJSON *err = cJSON_GetObjectItem(msg, "error");
            const cJSON *em = err ? cJSON_GetObjectItem(err, "message") : nullptr;
            LogBuffer::logfSerial("HA WS: command %d failed: %s", rid,
                (em && cJSON_IsString(em)) ? em->valuestring : "?");
        }
        const cJSON *result = cJSON_GetObjectItem(msg, "result");
        if (rid == s_idRegistry) {
            if (cJSON_IsArray(result)) {
                // entity registry: keep sensor.* entities with a device_id
                if (!s_entities) {
                    void *mem = heap_caps_malloc(sizeof(EntityMap),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    s_entities = mem ? new (mem) EntityMap() : new EntityMap();
                } else {
                    s_entities->clear();
                }
                int kept = 0;
                const cJSON *el;
                cJSON_ArrayForEach(el, result) {
                    const cJSON *eid = cJSON_GetObjectItem(el, "entity_id");
                    const cJSON *did = cJSON_GetObjectItem(el, "device_id");
                    if (!cJSON_IsString(eid) || !eid->valuestring) continue;
                    if (strncmp(eid->valuestring, "sensor.", 7) != 0) continue;
                    if (cJSON_IsString(did) && did->valuestring && did->valuestring[0])
                        (*s_entities)[eid->valuestring] = did->valuestring;
                    kept++;
                }
                LogBuffer::logfSerial("HA WS: registry: %d sensor entities", kept);
            {
                int shown = 0;
                const cJSON *e2;
                cJSON_ArrayForEach(e2, result) {
                    if (shown >= 3) break;
                    const cJSON *eid = cJSON_GetObjectItem(e2, "entity_id");
                    if (cJSON_IsString(eid) && eid->valuestring &&
                        strncmp(eid->valuestring, "sensor.", 7) == 0) {
                        LogBuffer::logfSerial("HA WS: e.g. %s", eid->valuestring);
                        shown++;
                    }
                }
            }
            } else {
                LogBuffer::logfSerial("HA WS: registry unavailable, per-entity mode");
            }
            // initial snapshot (after the map when possible); live events
            // already flow, so a failure here degrades gracefully
            s_registryDone = true;
            statesTimerStop();
            requestStates();
        } else if (rid == s_idStates && cJSON_IsArray(result)) {
            int nT = 0, nH = 0, nB = 0, nR = 0, nTot = 0;
            const cJSON *el;
            cJSON_ArrayForEach(el, result) {
                nTot++;
                const cJSON *attr = cJSON_GetObjectItem(el, "attributes");
                const cJSON *dc = attr ? cJSON_GetObjectItem(attr, "device_class") : nullptr;
                if (!cJSON_IsString(dc) || !dc->valuestring) continue;
                if (strcmp(dc->valuestring, "temperature") == 0) nT++;
                else if (strcmp(dc->valuestring, "humidity") == 0) nH++;
                else if (strcmp(dc->valuestring, "battery") == 0) nB++;
                else if (strcmp(dc->valuestring, "signal_strength") == 0) nR++;
            }
            parseStateList(result);
            LogBuffer::logfSerial("HA WS: states: %d total T=%d H=%d B=%d R=%d",
                nTot, nT, nH, nB, nR);
            // corruption check after the heaviest PSRAM churn in the system
            // (registry + states DOMs). The periodic walk is gone (it broke
            // the RGB scanout with OPI bursts); this runs once per reconnect.
            if (!heap_caps_check_integrity_all(true))
                LogBuffer::logfSerial("HEAP INTEGRITY FAIL (after states)");
        }
    }
    cJSON_Delete(msg);
}

static void wsEventHandler(void *handler_args, esp_event_base_t base,
                           int32_t event_id, void *event_data) {
    auto *event = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_msgId = 1;
            s_authSent = false;
            LogBuffer::logfSerial("HA WS connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            if (s_connected) {
                s_connected = false;
                LogBuffer::logfSerial("HA WS disconnected");
            }
            break;
        case WEBSOCKET_EVENT_DATA: {
#if HA_WS_VERBOSE
            // liveness meter: proves the WS task dispatches (any opcode/len)
            {
                static uint32_t nEv = 0;
                if (++nEv % 25 == 0)
                    LogBuffer::logfSerial("HA WS: data ev #%u (op=%d len=%d)",
                        (unsigned)nEv, event->op_code, event->data_len);
            }
#endif
            // Deferred get_states (timer backstop): runs here on the WS task
            // where the client lock is ours recursively.
            if (s_statesPending) {
                s_statesPending = false;
                requestStates();
            }
            // Large frames arrive as multiple DATA events (one per 1KB read).
            // The lib reports op_code=0x1 (text) on EVERY chunk, so a new
            // message MUST NOT be assumed on 0x1 (that wiped the accumulator
            // on each chunk and the registry never completed). Frame start
            // is payload_offset==0; completion is by bytes: the header
            // documents payload_len as the TOTAL length and payload_offset
            // as this event's offset within it.
            if (event->op_code != 0x1 && event->op_code != 0x0) break;
            if (event->payload_offset == 0) {
                // route by envelope id: the registry result streams
                // element-wise (never accumulated), everything else keeps
                // the legacy accumulate-then-DOM path below (incl. error
                // responses, which have no result array to stream)
                streamReset();
                int fid = -1;
                const char *dp = event->data_ptr;
                int dl = event->data_len;
                bool head = dl > 0 && dp && s_idRegistry != 0
                    && rawIntVal(dp, dl, "id", fid) && fid == s_idRegistry;
                bool arr = false;
                if (head) {
                    // the "[" after "result" must sit in the head chunk
                    // (the ~60B envelope always does); else stay legacy
                    int n = dl < 256 ? dl : 256;
                    for (int i = 0; i + 9 < n && !arr; i++) {
                        if (memcmp(dp + i, "\"result\"", 8) != 0) continue;
                        int j = i + 8;
                        while (j < n && (dp[j] == ' ' || dp[j] == '\t')) j++;
                        if (j < n && dp[j] == ':') {
                            j++;
                            while (j < n && (dp[j] == ' ' || dp[j] == '\t')) j++;
                            arr = (j < n && dp[j] == '[');
                        }
                    }
                }
                if (head && arr) {
                    s_streamActive = true;
                    s_ssPhase = StreamPhase::SS_SEEK_ARRAY;
                    if (!s_entities) {
                        void *mem = heap_caps_malloc(sizeof(EntityMap),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        s_entities = mem ? new (mem) EntityMap() : new EntityMap();
                    } else {
                        s_entities->clear();
                    }
                }
            }
            if (s_streamActive && !s_streamDead) {
                streamRegistryChunk(event->data_ptr, event->data_len);
                if (s_streamActive) {
                    // array-close normally finishes inside the chunk above;
                    // a truncated tail falls back to the states timer
                    bool last = event->payload_len > 0
                        && (event->payload_offset + event->data_len >= event->payload_len);
                    if (last) {
                        LogBuffer::logfSerial("HA WS: registry truncated, states will follow");
                        streamReset();
                    }
                }
                break;
            }
            if (event->payload_offset == 0) s_frag.clear(); // new frame starts (legacy)
            if (event->data_len > 0 && event->data_ptr) {
                if (s_frag.size() + event->data_len > (size_t)(1024 * 1024)) {
                    LogBuffer::logfSerial("HA WS: frame too big, dropping");
                    s_frag.clear();
                    break;
                }
                s_frag.append(event->data_ptr, event->data_len);
#if HA_WS_VERBOSE
                // dribble meter for bulk responses (registry/states)
                if (s_frag.size() > 4096) {
                    static int chunkNo = 0;
                    if (++chunkNo % 25 == 0)
                        LogBuffer::logfSerial("HA WS: big frame +%dB total %uB",
                            event->data_len, (unsigned)s_frag.size());
                }
#endif
            }
            bool last;
            if (event->payload_len > 0)
                last = (event->payload_offset + event->data_len
                        >= event->payload_len);
            else
                last = !s_frag.empty(); // legacy single-read messages (acks)
            if (last && !s_frag.empty()) {
#if HA_WS_VERBOSE
                LogBuffer::logfSerial("HA WS: frame complete %uB",
                    (unsigned)s_frag.size());
#endif
                handleMessage(s_frag.c_str(), (int)s_frag.size());
                s_frag.clear();
            }
            break;
        }
        default:
            break;
    }
}

void HAWebSocket::setWatchedCells(const std::vector<std::string> &cells) {
    s_watched.clear();
    for (auto &c : cells) {
        if (!c.empty()) s_watched.emplace_back(c.c_str());
    }
    s_watchAll = s_watched.empty();
}

void HAWebSocket::start(const AppConfig &config) {
    stop();
    setWatchedCells(config.cells);
    if (config.ha_url.empty() || config.ha_token.empty()) {
        LogBuffer::logfSerial("HA disabled (no url/token)");
        return;
    }
    std::string url = config.ha_url;
    if (url.compare(0, 5, "ws://") != 0) {
        LogBuffer::logfSerial("HA WS: only plain ws:// supported, got '%s' (ignoring)", url.c_str());
        return;
    }

    esp_websocket_client_config_t ws_cfg = {};
    s_token = config.ha_token;
    ws_cfg.uri = url.c_str();
    ws_cfg.task_stack = 8192;
    ws_cfg.reconnect_timeout_ms = 10000;
    ws_cfg.network_timeout_ms = 10000;
    s_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                  wsEventHandler, nullptr);
    esp_websocket_client_start(s_client);
    LogBuffer::logf("HA WS client started: %s", url.c_str());
}

void HAWebSocket::stop() {
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
        s_connected = false;
    }
    // free everything allocated for this source (lazy-memory rule)
    statesTimerStop();
    s_registryDone = false;
    s_statesPending = false;
    s_authed = false;
    s_authSent = false;
    s_authSentMs = 0;
    s_msgId = 1;
    s_idRegistry = 0;
    s_idStates = 0;
    s_watched.clear();
    s_watchAll = true;
    s_token.clear();
    s_token.shrink_to_fit();
    s_frag.clear();
    s_frag.shrink_to_fit();
    streamReset();
    s_tail.shrink_to_fit();
    if (s_entities) {
        s_entities->~EntityMap();
        heap_caps_free(s_entities);
        s_entities = nullptr;
    }
}

bool HAWebSocket::isConnected() { return s_connected; }