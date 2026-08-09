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
    bool   streaming = false;
};

// Server-side global state, derived from GET /api/virtuals. `paused` is the
// top-level field; brightness/mirror/flip are read from the first active
// effect's config (apply_global keeps these in sync across active effects).
struct GlobalsState {
    bool valid      = false;  // the response parsed
    bool paused     = false;
    bool has_effect = false;  // an active effect supplied brightness/mirror/flip
    int  brightness = 100;    // 0..100 (%)
    bool mirror     = false;
    bool flip       = false;
};

class LedFxClient {
public:
    LedFxClient() = default;
    void set_base_url(const String &url) { _base = url; }

    bool connect(const String &user, const String &pass);   // performs /api/auth/login

    // Returns 0 on success, HTTP status on failure.
    int fetch_scenes(SceneInfo *&out, int &count);
    // When `globals` is provided, it is also filled from the same response.
    int fetch_virtuals(VirtualInfo *&out, int &count, GlobalsState *globals = nullptr);

    int activate_scene(const String &id);
    int deactivate_scene(const String &id);

    int set_virtual_active(const String &id, bool active);
    int randomize_virtual(const String &id);

    int clear_all_effects();
    int apply_global(const String &json);

    // Pause / resume every virtual's output (Virtuals toolbar toggle).
    int pause_all(bool paused);

    // Apply one of the built-in LedFx gradient presets (Global screen).
    int set_gradient(const String &name);

    bool is_connected() const { return net.has_token(); }

private:
    String _base;
    String _url(const String &path) const { return _base + path; }
};

extern LedFxClient g_ledfx;
