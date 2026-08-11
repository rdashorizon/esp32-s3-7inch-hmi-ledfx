// ---------------------------------------------------------------------------
// ledfx.h — typed wrapper over the LedFx REST API
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "net.h"   // LoginStatus, and the `net` singleton the calls run over

// Upper bound on the scenes / virtuals we keep from a response. A typical
// LedFx install has at most a handful of either. The Scenes grid sizes its
// row descriptor from this (see ui_scenes.cpp), so raising it means revisiting
// that array too.
static const int LEDFX_MAX_ITEMS = 32;

struct SceneInfo {
    String id;
    String name;
    bool   active = false;
};

struct VirtualInfo {
    String id;
    String name;
    bool   active = false;
    String effect_type;        // empty if no effect
    String effect_name;
    String gradient;           // active gradient preset name, if the effect uses one
    bool   streaming = false;
};

// Server-side global state. `paused` and mirror/flip come from GET /api/virtuals
// (paused is top-level; mirror/flip from the first active effect). `brightness`
// is LedFx's master global_brightness from GET /api/config — the value the main
// LedFx brightness slider drives (frame *= max_brightness * global_brightness).
struct GlobalsState {
    bool valid          = false;  // /api/virtuals parsed
    bool paused         = false;
    bool has_flags      = false;  // an active effect supplied mirror/flip
    bool mirror         = false;
    bool flip           = false;
    bool has_brightness = false;  // global_brightness read from /api/config
    int  brightness     = 100;    // 0..100 (%)
};

class LedFxClient {
public:
    LedFxClient() = default;
    void set_base_url(const String &url) { _base = url; }

    // Performs /api/auth/login and returns the outcome (see LoginStatus in net.h).
    LoginStatus connect(const String &user, const String &pass);

    // Every call below returns the HTTP status code: 200 on success, the
    // server's code on an HTTP failure, or 500 when the response couldn't be
    // parsed / allocated.
    //
    // fetch_scenes / fetch_virtuals allocate `out` with new[] and hand
    // ownership to the caller, which must release it with delete[] (the
    // structs hold String members, so free() would leak). At most
    // LEDFX_MAX_ITEMS entries are returned.
    int fetch_scenes(SceneInfo *&out, int &count);
    // When `globals` is provided, it is also filled from the same response.
    int fetch_virtuals(VirtualInfo *&out, int &count, GlobalsState *globals = nullptr);

    int activate_scene(const String &id);
    int deactivate_scene(const String &id);

    int set_virtual_active(const String &id, bool active);
    int randomize_virtual(const String &id);

    // Set just the background_color of a virtual's currently-active effect,
    // preserving all other config. `type` must match the virtual's active
    // effect (the caller reads it from VirtualInfo.effect_type). `color_hex`
    // is a CSS-style color string (e.g. "#ff8040"); LedFx validates it.
    int set_virtual_color(const String &id, const String &type, const String &color_hex);

    int clear_all_effects();
    int apply_global(const String &json);

    // Pause / resume every virtual's output (Virtuals toolbar toggle).
    int pause_all(bool paused);

    // Apply one of the built-in LedFx gradient presets (Global screen).
    int set_gradient(const String &name);

    // LedFx master brightness (config global_brightness, 0..100 %).
    int set_global_brightness(int pct);
    int fetch_global_brightness(int &pct);

private:
    String _base;
    String _url(const String &path) const { return _base + path; }
    // Serialize + PUT. `resp_out` is only needed by callers that inspect the
    // response body (the /api/effects bulk actions).
    int _put(const String &path, const JsonDocument &doc, String *resp_out = nullptr);
};

extern LedFxClient g_ledfx;
