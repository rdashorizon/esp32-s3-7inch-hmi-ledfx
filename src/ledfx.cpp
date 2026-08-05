// ---------------------------------------------------------------------------
// ledfx.cpp — LedFx REST client
//
// fetch_scenes/fetch_virtuals allocate the result arrays with new[]. The
// caller owns them and must release them with delete[] (they hold String
// members, so free() would leak). Counts are capped at 32 scenes / 32
// virtuals — the typical LedFx install has at most a handful of either.
// ---------------------------------------------------------------------------
#include "ledfx.h"
#include "net.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

LedFxClient g_ledfx;

// The scenes/virtuals responses can be large. Parsing them into a
// StaticJsonDocument placed these multi-kilobyte buffers on the loop task's
// ~8 KB stack (the virtuals doc alone was 16 KB — a guaranteed overflow). Back
// the documents with PSRAM instead, so nothing large lands on the stack.
struct SpiRamAllocator {
    void *allocate(size_t size)   { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *ptr)   { heap_caps_free(ptr); }
    void *reallocate(void *ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

bool LedFxClient::connect(const String &user, const String &pass) {
    return net.login(_base, user, pass);
}

int LedFxClient::fetch_scenes(SceneInfo *&out, int &count) {
    out = nullptr;
    count = 0;
    String body;
    int code = net.get(_url("/api/scenes"), body);
    if (code != 200) return code;

    SpiRamJsonDocument doc(8192);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 500;

    JsonObject scenes = doc["scenes"];
    if (scenes.isNull()) return 200;

    count = 0;
    for (JsonPair kv : scenes) count++;
    if (count == 0) return 200;
    if (count > 32) count = 32;

    // new[] so the String members are constructed (and destructed on delete[]);
    // the previous calloc()/free() pair leaked every String on each refresh.
    out = new (std::nothrow) SceneInfo[count];
    if (!out) { count = 0; return 500; }
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

    SpiRamJsonDocument doc(16384);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 500;

    JsonObject virtuals = doc["virtuals"];
    if (virtuals.isNull()) return 200;

    count = 0;
    for (JsonPair kv : virtuals) count++;
    if (count == 0) return 200;
    if (count > 32) count = 32;

    out = new (std::nothrow) VirtualInfo[count];
    if (!out) { count = 0; return 500; }
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

int LedFxClient::pause_all(bool paused) {
    StaticJsonDocument<64> doc;
    doc["paused"] = paused;
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/virtuals"), body, resp);
}

int LedFxClient::set_gradient(const String &name) {
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["gradient"] = name;
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/effects"), body, resp);
}
