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

// Smoke-test the SPIRAM backing path at first use: we route every large JSON
// doc through MALLOC_CAP_SPIRAM, and silently falling back to internal RAM on a
// misconfigured board would re-introduce the stack-overflow class of bug. The
// assertion runs exactly once per boot.
static void assert_spiram_allocator_works_once() {
    static bool asserted = false;
    if (asserted) return;
    asserted = true;
    void *p = heap_caps_malloc(64, MALLOC_CAP_SPIRAM);
    if (!p) {
        Serial.println("[ledfx] FATAL: heap_caps_malloc(MALLOC_CAP_SPIRAM) failed; "
                       "PSRAM-backed JSON docs will not work.");
        // Don't return — let the rest of the code demonstrate the failure rather
        // than masking it. The next allocate() will return nullptr and the
        // deserializer will return NoMemory, which the caller surfaces as 500.
    } else {
        heap_caps_free(p);
    }
}

LoginStatus LedFxClient::connect(const String &user, const String &pass) {
    return net.login(_base, user, pass);
}

int LedFxClient::fetch_scenes(SceneInfo *&out, int &count) {
    assert_spiram_allocator_works_once();
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

int LedFxClient::fetch_virtuals(VirtualInfo *&out, int &count, GlobalsState *globals) {
    assert_spiram_allocator_works_once();
    out = nullptr;
    count = 0;
    String body;
    int code = net.get(_url("/api/virtuals"), body);
    if (code != 200) return code;

    SpiRamJsonDocument doc(16384);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 500;

    if (globals) {
        globals->valid  = true;
        globals->paused = doc["paused"] | false;  // top-level global pause state
    }

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
            // Effect's gradient preset (if the effect uses one). Used by the
            // Virtuals UI to color the per-row swatch. Some effects (e.g.
            // SingleColor) don't have a gradient; the field is left empty.
            JsonObject ec = eff["config"];
            if (!ec.isNull()) {
                out[i].gradient = ec["gradient"] | "";
            }
            // Take mirror/flip from the first active effect as the
            // representative values (apply_global keeps them in sync). Brightness
            // is not here — it's the master global_brightness in /api/config.
            if (globals && !globals->has_flags && (kv.value()["active"] | false)) {
                JsonObject ec = eff["config"];
                if (!ec.isNull()) {
                    globals->mirror    = ec["mirror"] | false;
                    globals->flip      = ec["flip"] | false;
                    globals->has_flags = true;
                }
            }
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

// LedFx's /api/effects bulk actions (apply_global) return HTTP 200 even when
// they reject the request, with {"status":"failed", ...} in the body. Treat
// that as an error so the UI can report it instead of a false success.
static int interpret_effects_result(int code, const String &resp) {
    if (code != 200) return code;
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
        const char *status = doc["status"] | "";
        if (strcmp(status, "failed") == 0) return 422;
    }
    return 200;
}

int LedFxClient::apply_global(const String &json) {
    String resp;
    int code = net.put_json(_url("/api/effects"), json, resp);
    return interpret_effects_result(code, resp);
}

int LedFxClient::pause_all(bool paused) {
    StaticJsonDocument<64> doc;
    doc["paused"] = paused;
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/virtuals"), body, resp);
}

int LedFxClient::set_global_brightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    StaticJsonDocument<64> doc;
    doc["global_brightness"] = pct / 100.0f;
    String body;
    serializeJson(doc, body);
    String resp;
    return net.put_json(_url("/api/config"), body, resp);
}

int LedFxClient::fetch_global_brightness(int &pct) {
    String body;
    int code = net.get(_url("/api/config"), body);
    if (code != 200) return code;
    // Filter so only global_brightness is parsed — the full config is large.
    StaticJsonDocument<64> filter;
    filter["global_brightness"] = true;
    StaticJsonDocument<96> doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
        return 500;
    float gb = doc["global_brightness"] | 1.0f;
    pct = (int)(gb * 100.0f + 0.5f);
    return 200;
}

int LedFxClient::set_gradient(const String &name) {
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["gradient"] = name;
    String body;
    serializeJson(doc, body);
    String resp;
    int code = net.put_json(_url("/api/effects"), body, resp);
    return interpret_effects_result(code, resp);
}
