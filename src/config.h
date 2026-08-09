// ---------------------------------------------------------------------------
// config.h — NVS persistence + captive portal
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

struct Config {
    String wifi_ssid;
    String wifi_pass;
    String ledfx_url;       // e.g. "http://192.168.1.20:8888"
    String ledfx_user;
    String ledfx_pass;
};

// RGB triple used by the Color picker tab (and any future color-aware
// feature). Persisted to NVS by ui_color_apply_now() so the device restores
// the last-picked color on boot.
//
// Named `LedColor` rather than the more obvious `RGBColor` to avoid colliding
// with LovyanGFX's RGBColor struct (which has r/g/b members but also R8/G8/B8
// bitfield accessors, etc.).
struct LedColor {
    uint8_t r, g, b;
};

class ConfigStore {
public:
    bool load(Config &out);
    void save(const Config &cfg);
    void clear();

    // Panel backlight level (0..100 %), persisted independently of the WiFi/
    // LedFx config so it survives reboots. Returns `def` when never set.
    uint8_t load_screen_brightness(uint8_t def = 100);
    void    save_screen_brightness(uint8_t pct);

    // Last-selected tab index (0 = Scenes, 1 = Virtuals, 2 = Color, 3 = Global).
    // Returns `def` when never set.
    uint8_t load_last_tab(uint8_t def = 0);
    void    save_last_tab(uint8_t idx);

    // Last picked color (RGB 0..255 each). Defaults to warm white when unset.
    LedColor load_last_color();
    void     save_last_color(LedColor c);

    // Last color picked in the per-virtual color modal (Tier 2.2). Single
    // global value (not per-virtual — see plan Risks for why). Used as the
    // modal's seed RGB so the picker opens at the user's last choice instead
    // of re-deriving from the virtual's gradient.
    LedColor load_last_virt_color();
    void     save_last_virt_color(LedColor c);

    // Captive portal DNS+webserver. Blocks until the user submits a config
    // or until timeout_seconds elapses. Returns true if a config was saved.
    bool run_captive_portal(uint32_t timeout_seconds = 0);

    bool has_wifi() const { return _has_wifi; }
    bool has_ledfx() const { return _has_ledfx; }

private:
    bool _has_wifi = false;
    bool _has_ledfx = false;
};

// URL sanity check — must start with http:// or https:// and have at least one
// character of host. Used by the captive portal and the on-device settings
// editor to reject garbage before rebooting.
bool config_url_is_valid(const String &url);

extern ConfigStore config_store;
