// ---------------------------------------------------------------------------
// net.h — WiFi + HTTPClient with bearer-token auth
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <WiFi.h>        // wifi_connected() calls WiFi.status() inline below
#include <HTTPClient.h>

// Login outcome. Distinguishing these lets the UI surface "wrong password"
// (action needed) from "mainline LedFx has no auth endpoint" (silent — the
// data fetches will just work tokenless). Note: NOT_CONFIGURED is named to
// avoid colliding with the Arduino framework's #define DISABLED.
enum class LoginStatus {
    OK,                // 200 + token returned
    NOT_CONFIGURED,    // no credentials configured — don't even try
    NOT_FOUND,         // 404 — mainline LedFx, no /api/auth/login endpoint
    BAD_CREDS,         // 401/403 — wrong username/password (user action needed)
    TRANSPORT,         // connect failed, timeout, DNS, etc.
    INVALID_RESPONSE,  // 200 but body wasn't parseable as JSON / no token field
};

// Bounds every HTTP call (TCP connect + per-read), so a dead or slow LedFx
// server can never stall a request indefinitely. Exposed because the worker's
// one-shot connection probe builds its own HTTPClient and must use the same
// budget.
static const uint16_t NET_HTTP_TIMEOUT_MS = 6000;

class Net {
public:
    bool connect_wifi(const String &ssid, const String &pass, uint32_t timeout_ms = 15000);
    bool wifi_connected() const { return WiFi.status() == WL_CONNECTED; }

    // Login against the LedFx server. Returns the outcome; on OK the bearer
    // token is cached for subsequent calls. Credentials are also cached so a
    // later 401/403 can trigger a re-auth attempt.
    LoginStatus login(const String &base_url, const String &user, const String &pass);

    bool has_token() const { return _token.length() > 0; }
    const String &token() const { return _token; }

    // GET/PUT helpers that inject the bearer token. On a 401/403 they
    // transparently re-login once with the cached credentials and retry.
    int get(const String &url, String &body);
    int put_json(const String &url, const String &json, String &body);

private:
    // One HTTP request with bearer auth + single re-auth-on-401 retry.
    int request(const char *method, const String &url, const String &body, String &resp);

    String _token;
    // Cached so request() can re-authenticate when the token expires.
    String _base;
    String _user;
    String _pass;
};

extern Net net;
