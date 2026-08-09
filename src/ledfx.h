// ---------------------------------------------------------------------------
// ledfx.h — typed wrapper over the LedFx REST API
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "net.h"   // for the `net` singleton used by is_connected()

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

    // Returns 0 on success, HTTP status on failure.
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

    bool is_connected() const { return net.has_token(); }

private:
    String _base;
    String _url(const String &path) const { return _base + path; }
};

extern LedFxClient g_ledfx;
