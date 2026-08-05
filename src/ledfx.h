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

class LedFxClient {
public:
    LedFxClient() = default;
    void set_base_url(const String &url) { _base = url; }

    bool connect(const String &user, const String &pass);   // performs /api/auth/login

    // Returns 0 on success, HTTP status on failure.
    int fetch_scenes(SceneInfo *&out, int &count);
    int fetch_virtuals(VirtualInfo *&out, int &count);

    int activate_scene(const String &id);
    int deactivate_scene(const String &id);

    int set_virtual_active(const String &id, bool active);
    int randomize_virtual(const String &id);

    int clear_all_effects();
    int apply_global(const String &json);

    bool is_connected() const { return net.has_token(); }

private:
    String _base;
    String _url(const String &path) const { return _base + path; }
};

extern LedFxClient g_ledfx;
