// ---------------------------------------------------------------------------
// ledfx.cpp — LedFx REST client
//
// All public methods allocate the result arrays on the heap. The caller
// must free() them. Sizes are deliberately small (cap at 32 scenes / 32
// virtuals) — the typical LedFx install has at most a handful of either.
// ---------------------------------------------------------------------------
#include "ledfx.h"
#include "net.h"
#include <ArduinoJson.h>

LedFxClient g_ledfx;

bool LedFxClient::connect(const String &user, const String &pass) {
    return net.login(_base, user, pass);
}

int LedFxClient::fetch_scenes(SceneInfo *&out, int &count) {
    out = nullptr;
    count = 0;
    String body;
    int code = net.get(_url("/api/scenes"), body);
    if (code != 200) return code;

    StaticJsonDocument<8192> doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 500;

    JsonObject scenes = doc["scenes"];
    if (scenes.isNull()) return 200;

    count = 0;
    for (JsonPair kv : scenes) count++;
    if (count == 0) return 200;
    if (count > 32) count = 32;

    out = (SceneInfo *)calloc(count, sizeof(SceneInfo));
    int i = 0;
    for (JsonPair kv : scenes) {
        if (i >= count) break;
        out[i].id = kv.key().c_str();
        out[i].name = kv.value()["name"] | kv.key().c_str();
        out[i].active = kv.value()["active"] | false;
        i++;
    }
    return 200;
}

int LedFxClient::fetch_virtuals(VirtualInfo *&out, int &count) {
    out = nullptr;
    count = 0;
    String body;
    int code = net.get(_url("/api/virtuals"), body);
    if (code != 200) return code;

    StaticJsonDocument<16384> doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 500;

    JsonObject virtuals = doc["virtuals"];
    if (virtuals.isNull()) return 200;

    count = 0;
    for (JsonPair kv : virtuals) count++;
    if (count == 0) return 200;
    if (count > 32) count = 32;

    out = (VirtualInfo *)calloc(count, sizeof(VirtualInfo));
    int i = 0;
    for (JsonPair kv : virtuals) {
        if (i >= count) break;
        out[i].id = kv.key().c_str();
        out[i].name = kv.value()["config"]["name"] | kv.key().c_str();
        out[i].active = kv.value()["active"] | false;
        out[i].streaming = kv.value()["streaming"] | false;
        JsonObject eff = kv.value()["effect"];
        if (!eff.isNull()) {
            out[i].effect_type = eff["type"] | "";
            out[i].effect_name = eff["name"] | "";
        }
        i++;
    }
    return 200;
}

int LedFxClient::activate_scene(const String &id) {
    StaticJsonDocument<128> doc;
    doc["id"] = id;
    doc["action"] = "activate";
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/scenes"), body, resp);
}

int LedFxClient::deactivate_scene(const String &id) {
    StaticJsonDocument<128> doc;
    doc["id"] = id;
    doc["action"] = "deactivate";
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/scenes"), body, resp);
}

int LedFxClient::set_virtual_active(const String &id, bool active) {
    StaticJsonDocument<128> doc;
    doc["active"] = active;
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/virtuals/" + id), body, resp);
}

int LedFxClient::randomize_virtual(const String &id) {
    StaticJsonDocument<128> doc;
    doc["config"] = "RANDOMIZE";
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/virtuals/" + id + "/effects"), body, resp);
}

int LedFxClient::clear_all_effects() {
    StaticJsonDocument<64> doc;
    doc["action"] = "clear_all_effects";
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/effects"), body, resp);
}

int LedFxClient::apply_global(const String &json) {
    String resp;
    return net.put_json(_url("/api/effects"), json, resp);
}
