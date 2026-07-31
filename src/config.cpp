// ---------------------------------------------------------------------------
// config.cpp — NVS persistence + first-run captive portal
//
// We use the ESP32 Preferences API (NVS) rather than the filesystem so the
// settings survive a reflash and follow the board. The captive portal is a
// minimal WebServer on the chip's AP `ledfx-hmi-setup`. The DNS server
// rewrites all queries to the chip's IP so the phone pops up the form
// automatically on most devices.
// ---------------------------------------------------------------------------
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

static const char *NVS_NS = "ledfx-hmi";
static const char *K_SSID = "wifi_ssid";
static const char *K_WPASS = "wifi_pass";
static const char *K_URL  = "ledfx_url";
static const char *K_USER = "ledfx_user";
static const char *K_LPASS = "ledfx_pass";
static const char *K_AP_TOUT = "ap_timeout";

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(4, 3, 2, 1);

ConfigStore config_store;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><title>LedFX HMI Setup</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:system-ui;max-width:480px;margin:2em auto;padding:0 1em;color:#222}
h1{font-size:1.4em}
label{display:block;margin:0.8em 0 0.2em;font-weight:600}
input{width:100%;padding:0.5em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}
button{margin-top:1.2em;padding:0.7em 1.4em;background:#0066cc;color:#fff;border:0;border-radius:4px;font-size:1em;cursor:pointer}
small{color:#666}
</style></head><body>
<h1>LedFX HMI — Setup</h1>
<form action="/save" method="POST">
  <label>WiFi SSID</label><input name="wifi_ssid" required>
  <label>WiFi Password</label><input name="wifi_pass" type="password">
  <label>LedFx URL</label><input name="ledfx_url" placeholder="http://192.168.1.20:8888" required>
  <label>LedFx Username <small>(optional)</small></label><input name="ledfx_user">
  <label>LedFx Password <small>(optional)</small></label><input name="ledfx_pass" type="password">
  <button type="submit">Save &amp; reboot</button>
</form>
</body></html>
)HTML";

bool ConfigStore::load(Config &out) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    out.wifi_ssid = prefs.getString(K_SSID, "");
    out.wifi_pass = prefs.getString(K_WPASS, "");
    out.ledfx_url = prefs.getString(K_URL, "");
    out.ledfx_user = prefs.getString(K_USER, "");
    out.ledfx_pass = prefs.getString(K_LPASS, "");
    prefs.end();
    _has_wifi = out.wifi_ssid.length() > 0;
    _has_ledfx = out.ledfx_url.length() > 0;
    return _has_wifi && _has_ledfx;
}

void ConfigStore::save(const Config &cfg) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(K_SSID, cfg.wifi_ssid);
    prefs.putString(K_WPASS, cfg.wifi_pass);
    prefs.putString(K_URL,  cfg.ledfx_url);
    prefs.putString(K_USER, cfg.ledfx_user);
    prefs.putString(K_LPASS, cfg.ledfx_pass);
    prefs.end();
    _has_wifi = cfg.wifi_ssid.length() > 0;
    _has_ledfx = cfg.ledfx_url.length() > 0;
}

void ConfigStore::clear() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
    _has_wifi = _has_ledfx = false;
}

bool ConfigStore::run_captive_portal(uint32_t timeout_seconds) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("ledfx-hmi-setup", "");  // open AP

    DNSServer dns;
    dns.start(DNS_PORT, "*", AP_IP);

    WebServer server(80);
    Config cfg;

    server.onNotFound([]() { server.send(200, "text/html", INDEX_HTML); });
    server.on("/", HTTP_GET, []() { server.send(200, "text/html", INDEX_HTML); });
    server.on("/generate_204", HTTP_GET, []() { server.send(200, "text/html", INDEX_HTML); });

    server.on("/save", HTTP_POST, [this, &cfg, &server]() {
        cfg.wifi_ssid = server.arg("wifi_ssid");
        cfg.wifi_pass = server.arg("wifi_pass");
        cfg.ledfx_url = server.arg("ledfx_url");
        cfg.ledfx_user = server.arg("ledfx_user");
        cfg.ledfx_pass = server.arg("ledfx_pass");
        this->save(cfg);
        server.send(200, "text/html", "<h1>Saved</h1><p>Rebooting...</p>");
        delay(500);
        ESP.restart();
    });

    server.begin();

    uint32_t start = millis();
    while (true) {
        dns.processNextRequest();
        server.handleClient();
        delay(10);
        if (timeout_seconds > 0 && (millis() - start) > timeout_seconds * 1000UL) {
            return false;
        }
    }
}
