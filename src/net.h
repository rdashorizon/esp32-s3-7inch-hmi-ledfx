// ---------------------------------------------------------------------------
// net.h — WiFi + HTTPClient with bearer-token auth
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

class Net {
public:
    bool connect_wifi(const String &ssid, const String &pass, uint32_t timeout_ms = 15000);
    bool wifi_connected() const { return WiFi.status() == WL_CONNECTED; }

    // Login against the LedFx server. Returns true on success and caches
    // the bearer token for subsequent calls.
    bool login(const String &base_url, const String &user, const String &pass);

    bool has_token() const { return _token.length() > 0; }
    const String &token() const { return _token; }

    // Get/POST/PUT helpers that inject the bearer token.
    int get(const String &url, String &body);
    int post_json(const String &url, const String &json, String &body);
    int put_json(const String &url, const String &json, String &body);

private:
    String _token;
};

extern Net net;
