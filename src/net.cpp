// ---------------------------------------------------------------------------
// net.cpp — WiFi + bearer-token HTTP client
//
// LedFx v2 issues a short-lived bearer token via `POST /api/auth/login`.
// We cache the token in this module and re-inject it on every call.
// The token survives reboots only via re-login — we do not store it
// in NVS because the token is short-lived by design.
// ---------------------------------------------------------------------------
#include "net.h"
#include <WiFi.h>
#include <ArduinoJson.h>

Net net;

// Bound every HTTP call so a dead or slow LedFx server can never stall the
// loop indefinitely (TCP connect + per-read timeout, in milliseconds).
static const uint16_t HTTP_TIMEOUT_MS = 6000;

bool Net::connect_wifi(const String &ssid, const String &pass, uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) return false;
        delay(100);
    }
    return true;
}

LoginStatus Net::login(const String &base_url, const String &user, const String &pass) {
    // Cache credentials so request() can re-auth on token expiry.
    _base = base_url;
    _user = user;
    _pass = pass;
    if (WiFi.status() != WL_CONNECTED) return LoginStatus::TRANSPORT;

    HTTPClient http;
    String url = base_url + "/api/auth/login";
    http.begin(url);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    if (user.length() > 0) doc["username"] = user;
    if (pass.length() > 0) doc["password"] = pass;
    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    String resp = http.getString();
    http.end();

    // Map HTTP codes to LoginStatus. We only consider 200 OK here; everything
    // else is one of the failure modes.
    if (code == 404) { _token = ""; return LoginStatus::NOT_FOUND; }
    if (code == 401 || code == 403) { _token = ""; return LoginStatus::BAD_CREDS; }
    if (code != 200) { _token = ""; return LoginStatus::TRANSPORT; }

    // 200 OK — must have a token in the body.
    StaticJsonDocument<512> r;
    if (deserializeJson(r, resp) != DeserializationError::Ok) {
        _token = "";
        return LoginStatus::INVALID_RESPONSE;
    }
    _token = r["token"].as<String>();
    if (_token.isEmpty()) _token = r["access_token"].as<String>();
    if (_token.isEmpty()) {
        return LoginStatus::INVALID_RESPONSE;
    }
    return LoginStatus::OK;
}

int Net::request(const char *method, const String &url, const String &body, String &resp) {
    // Up to two attempts: if the first fails with 401/403 the token has likely
    // expired, so re-login once with the cached credentials and retry.
    for (int attempt = 0; attempt < 2; attempt++) {
        HTTPClient http;
        http.begin(url);
        http.setConnectTimeout(HTTP_TIMEOUT_MS);
        http.setTimeout(HTTP_TIMEOUT_MS);
        if (_token.length()) http.addHeader("Authorization", "Bearer " + _token);
        http.addHeader("Content-Type", "application/json");
        int code;
        if (strcmp(method, "GET") == 0) {
            code = http.GET();
        } else if (strcmp(method, "POST") == 0) {
            code = http.POST(body);
        } else {
            code = http.PUT(body);
        }
        resp = http.getString();
        http.end();

        if ((code == 401 || code == 403) && attempt == 0 && _base.length()) {
            _token = "";
            if (login(_base, _user, _pass) != LoginStatus::OK) return code;  // re-auth failed
            continue;                                        // retry once
        }
        return code;
    }
    return 0;
}

int Net::get(const String &url, String &body) {
    return request("GET", url, "", body);
}

int Net::post_json(const String &url, const String &json, String &body) {
    return request("POST", url, json, body);
}

int Net::put_json(const String &url, const String &json, String &body) {
    return request("PUT", url, json, body);
}
