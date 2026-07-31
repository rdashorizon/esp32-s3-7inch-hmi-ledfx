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

class ConfigStore {
public:
    bool load(Config &out);
    void save(const Config &cfg);
    void clear();

    // Captive portal DNS+webserver. Blocks until the user submits a config
    // or until timeout_seconds elapses. Returns true if a config was saved.
    bool run_captive_portal(uint32_t timeout_seconds = 0);

    bool has_wifi() const { return _has_wifi; }
    bool has_ledfx() const { return _has_ledfx; }

private:
    bool _has_wifi = false;
    bool _has_ledfx = false;
};

extern ConfigStore config_store;
