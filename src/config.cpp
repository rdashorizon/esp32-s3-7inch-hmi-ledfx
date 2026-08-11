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
#include "lvgl_port.h"   // keep the screen alive while the portal blocks
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
static const char *K_SCRBRI = "scr_bright";
static const char *K_LASTTAB = "last_tab";
static const char *K_LASTCLR = "last_color";
static const char *K_LASTVCLR = "last_vcolor";
static const char *K_THEME_MODE   = "theme_mode";
static const char *K_THEME_ACCENT = "theme_accent";

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(4, 3, 2, 1);

ConfigStore config_store;

// Captive-portal HTML template. The "{psk}" placeholder is replaced at
// request time with the per-device password (printed on the form so the user
// knows what to type when connecting to the AP).
static const char INDEX_HTML_TPL[] PROGMEM = R"HTML(
<!doctype html><html><head><title>LedFX HMI Setup</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:system-ui;max-width:480px;margin:2em auto;padding:0 1em;color:#222}
h1{font-size:1.4em}
.ap-info{background:#eef;border:1px solid #99c;border-radius:4px;padding:0.6em 0.8em;margin:1em 0;font-family:monospace}
.ap-info strong{font-size:1.1em}
label{display:block;margin:0.8em 0 0.2em;font-weight:600}
input{width:100%;padding:0.5em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}
button{margin-top:1.2em;padding:0.7em 1.4em;background:#0066cc;color:#fff;border:0;border-radius:4px;font-size:1em;cursor:pointer}
small{color:#666}
</style></head><body>
<h1>LedFX HMI — Setup</h1>
<p>Connect your phone or laptop to the WiFi network:</p>
<div class="ap-info">SSID: <strong>ledfx-hmi-setup</strong><br>Password: <strong>{psk}</strong></div>
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

// Render the captive-portal HTML for this specific device's PSK.
static String render_index_html(const String &psk) {
    String html = INDEX_HTML_TPL;
    html.replace("{psk}", psk);
    return html;
}

// Per-device identity: last 3 bytes of the efuse MAC as lowercase hex (6
// chars). Reproducible across reboots, unique per device. Single source of
// truth for the AP PSK, the OTA hostname, and the OTA password.
String config_device_hex() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[7];
    snprintf(buf, sizeof(buf), "%02x%02x%02x",
             (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
    return String(buf);
}

// The per-device AP password, printed to Serial and onto the captive-portal
// form so the user knows what to type when joining the AP.
//
// This MUST be the exact string passed to WiFi.softAP(). It previously
// returned the bare 6-hex device id while softAP() was given
// `hex + "hmi2026"`, so both the serial log and the form advertised a
// password that does not work. The suffix also clears WPA2's 8-character
// minimum, which the bare hex does not.
static String derive_ap_psk() {
    return config_ota_password();
}

// mDNS hostname advertised for network OTA: "ledfx-hmi-<hex>".
String config_ota_hostname() {
    return "ledfx-hmi-" + config_device_hex();
}

// The single per-device secret: 6-hex device id + a fixed suffix (so it clears
// WPA2's 8-character minimum). Guards both the setup AP and network
// reflashing — see derive_ap_psk().
String config_ota_password() {
    return config_device_hex() + "hmi2026";
}

bool config_url_is_valid(const String &url) {
    if (url.length() < 8) return false;  // shorter than "http://x"
    if (!url.startsWith("http://") && !url.startsWith("https://")) return false;
    int scheme_len = url.startsWith("https://") ? 8 : 7;
    // Require at least one host character before any path/query.
    return url.charAt(scheme_len) != '/' && url.charAt(scheme_len) != '\0';
}

bool ConfigStore::load(Config &out) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    out.wifi_ssid = prefs.getString(K_SSID, "");
    out.wifi_pass = prefs.getString(K_WPASS, "");
    out.ledfx_url = prefs.getString(K_URL, "");
    out.ledfx_user = prefs.getString(K_USER, "");
    out.ledfx_pass = prefs.getString(K_LPASS, "");
    prefs.end();
    return out.wifi_ssid.length() > 0 && out.ledfx_url.length() > 0;
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
}

void ConfigStore::clear() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
}

uint8_t ConfigStore::load_screen_brightness(uint8_t def) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    uint8_t pct = prefs.getUChar(K_SCRBRI, def);
    prefs.end();
    if (pct > 100) pct = 100;
    return pct;
}

void ConfigStore::save_screen_brightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(K_SCRBRI, pct);
    prefs.end();
}

uint8_t ConfigStore::load_last_tab(uint8_t def) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    uint8_t idx = prefs.getUChar(K_LASTTAB, def);
    prefs.end();
    // Returned raw. The valid range belongs to the UI (see UiTab in ui.h),
    // which clamps it — duplicating the bound here would just rot when a tab
    // is added.
    return idx;
}

void ConfigStore::save_last_tab(uint8_t idx) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(K_LASTTAB, idx);
    prefs.end();
}

LedColor ConfigStore::load_last_color() {
    // Default: warm white (255, 200, 128) so the LED strip is visible on
    // first boot rather than starting dark.
    uint8_t buf[3] = {255, 200, 128};
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    prefs.getBytes(K_LASTCLR, buf, sizeof(buf));
    prefs.end();
    return { buf[0], buf[1], buf[2] };
}

void ConfigStore::save_last_color(LedColor c) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    uint8_t buf[3] = { c.r, c.g, c.b };
    prefs.putBytes(K_LASTCLR, buf, sizeof(buf));
    prefs.end();
}

LedColor ConfigStore::load_last_virt_color() {
    // Same warm-white default as the global Color picker's last_color so a
    // brand-new device behaves the same way whether the user opens the global
    // tab or the per-virtual modal first.
    uint8_t buf[3] = {255, 200, 128};
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    prefs.getBytes(K_LASTVCLR, buf, sizeof(buf));
    prefs.end();
    return { buf[0], buf[1], buf[2] };
}

void ConfigStore::save_last_virt_color(LedColor c) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    uint8_t buf[3] = { c.r, c.g, c.b };
    prefs.putBytes(K_LASTVCLR, buf, sizeof(buf));
    prefs.end();
}

Theme config_load_theme() {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    Theme t;
    t.mode   = (ThemeMode)  prefs.getUChar(K_THEME_MODE,   (uint8_t)ThemeMode::DARK);
    t.accent = (AccentColor)prefs.getUChar(K_THEME_ACCENT, (uint8_t)AccentColor::BLUE);
    prefs.end();
    // Clamp to known values; corrupted NVS could land us anywhere.
    if ((uint8_t)t.mode   > (uint8_t)ThemeMode::LIGHT)    t.mode   = ThemeMode::DARK;
    if ((uint8_t)t.accent > (uint8_t)AccentColor::MAGENTA) t.accent = AccentColor::BLUE;
    return t;
}

void config_save_theme(const Theme &t) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(K_THEME_MODE,   (uint8_t)t.mode);
    prefs.putUChar(K_THEME_ACCENT, (uint8_t)t.accent);
    prefs.end();
}

bool ConfigStore::run_captive_portal(uint32_t timeout_seconds) {
    String psk = derive_ap_psk();
    Serial.printf("[portal] AP SSID='ledfx-hmi-setup' PSK='%s'\n", psk.c_str());

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    // WPA2 with the per-device PSK. Anyone nearby still has to know the
    // device-specific hex prefix to connect. Same string that was printed and
    // is rendered onto the form below — do not re-derive it here.
    WiFi.softAP("ledfx-hmi-setup", psk.c_str());

    DNSServer dns;
    dns.start(DNS_PORT, "*", AP_IP);

    WebServer server(80);
    Config cfg;

    auto send_index = [&server, &psk]() {
        server.send(200, "text/html", render_index_html(psk));
    };
    server.onNotFound(send_index);
    server.on("/", HTTP_GET, send_index);
    server.on("/generate_204", HTTP_GET, send_index);

    server.on("/save", HTTP_POST, [this, &cfg, &server]() {
        cfg.wifi_ssid = server.arg("wifi_ssid");
        cfg.wifi_pass = server.arg("wifi_pass");
        cfg.ledfx_url = server.arg("ledfx_url");
        cfg.ledfx_user = server.arg("ledfx_user");
        cfg.ledfx_pass = server.arg("ledfx_pass");
        if (!config_url_is_valid(cfg.ledfx_url)) {
            server.send(200, "text/html",
                "<h1>Invalid LedFx URL</h1>"
                "<p>The URL must start with <code>http://</code> or "
                "<code>https://</code> and include a host. Example: "
                "<code>http://192.168.1.20:8888</code>.</p>"
                "<p><a href=\"/\">Back to form</a></p>");
            return;
        }
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
        lvgl_tick();  // service the display so the setup screen stays responsive
        delay(5);
        if (timeout_seconds > 0 && (millis() - start) > timeout_seconds * 1000UL) {
            return false;
        }
    }
}
