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

bool Net::login(const String &base_url, const String &user, const String &pass) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    String url = base_url + "/api/auth/login";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    if (user.length() > 0) doc["username"] = user;
    if (pass.length() > 0) doc["password"] = pass;
    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    if (code != 200) {
        http.end();
        _token = "";
        return false;
    }
    String resp = http.getString();
    http.end();

    // LedFx returns either `{"token": "..."}` or `{"access_token": "..."}`.
    StaticJsonDocument<512> r;
    if (deserializeJson(r, resp) != DeserializationError::Ok) {
        _token = "";
        return false;
    }
    _token = r["token"].as<String>();
    if (_token.isEmpty()) _token = r["access_token"].as<String>();
    return _token.length() > 0;
}

static int do_request(const String &method, const String &url, const String &body,
                      const String &token, String &resp) {
    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("Content-Type", "application/json");
    int code;
    if (method == "GET") {
        code = http.GET();
    } else if (method == "POST") {
        code = http.POST(body);
    } else {
        code = http.PUT(body);
    }
    resp = http.getString();
    http.end();
    return code;
}

int Net::get(const String &url, String &body) {
    return do_request("GET", url, "", _token, body);
}

int Net::post_json(const String &url, const String &json, String &body) {
    return do_request("POST", url, json, _token, body);
}

int Net::put_json(const String &url, const String &json, String &body) {
    return do_request("PUT", url, json, _token, body);
}
