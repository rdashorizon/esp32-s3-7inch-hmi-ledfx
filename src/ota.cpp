// ---------------------------------------------------------------------------
// ota.cpp — network firmware updates (ArduinoOTA / espota)
//
// See ota.h for the threading contract. In short: ota_loop() runs on the LVGL
// thread (core 1), so the progress overlay is built and repainted from the
// ArduinoOTA callbacks directly; the background network worker (core 0) is
// suspended for the duration of the flash.
// ---------------------------------------------------------------------------
#include "ota.h"
#include "config.h"     // config_ota_hostname / config_ota_password
#include "display.h"    // display_set_backlight
#include "worker.h"     // worker_suspend / worker_resume
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <lvgl.h>

// ArduinoOTA is only started once WiFi is up; this guards that one-time begin().
static bool s_started = false;

// Progress overlay (top layer). Built on onStart, torn down on onError; onEnd
// reboots the device so it never needs explicit teardown.
static lv_obj_t *s_ovl   = nullptr;
static lv_obj_t *s_bar   = nullptr;
static lv_obj_t *s_pct   = nullptr;
// Throttle repaints: redrawing on every OTA packet slows the transfer and the
// bar can only move in 1% steps anyway.
static int s_last_pct = -1;

static void overlay_build() {
    if (s_ovl) return;
    s_ovl = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ovl, lv_color_hex(0x0a0a12), 0);
    lv_obj_set_style_bg_opa(s_ovl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ovl, 0, 0);
    lv_obj_clear_flag(s_ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ovl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ovl, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ovl, 16, 0);   // spacing between the stacked children

    lv_obj_t *title = lv_label_create(s_ovl);
    lv_label_set_text(title, LV_SYMBOL_DOWNLOAD "  Updating firmware");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    s_bar = lv_bar_create(s_ovl);
    lv_obj_set_size(s_bar, 500, 24);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_pct = lv_label_create(s_ovl);
    lv_label_set_text(s_pct, "0%");
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0xaaaaaa), 0);

    lv_obj_t *note = lv_label_create(s_ovl);
    lv_label_set_text(note, "Do not power off");
    lv_obj_set_style_text_color(note, lv_color_hex(0x888888), 0);
}

static void overlay_destroy() {
    if (s_ovl) { lv_obj_del(s_ovl); s_ovl = nullptr; s_bar = s_pct = nullptr; }
    s_last_pct = -1;
}

static void on_start() {
    // U_SPIFFS isn't used on this device (no data partition to flash), but the
    // overlay wording works for either; ArduinoOTA rejects a filesystem image
    // itself if the partition is absent.
    worker_suspend();           // stop core-0 HTTP churn during the flash
    display_set_backlight(255); // full brightness so progress is visible
    overlay_build();
    lv_refr_now(NULL);
    Serial.println("[ota] update starting");
}

static void on_progress(unsigned int progress, unsigned int total) {
    int pct = total ? (int)((progress * 100UL) / total) : 0;
    if (pct == s_last_pct) return;   // only repaint on a real 1% step
    s_last_pct = pct;
    if (s_bar) lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    if (s_pct) lv_label_set_text_fmt(s_pct, "%d%%", pct);
    // We're blocking inside ArduinoOTA.handle(), so the normal LVGL tick isn't
    // running — force the framebuffer to update here.
    lv_refr_now(NULL);
}

static void on_end() {
    if (s_pct) lv_label_set_text(s_pct, "Complete — rebooting");
    lv_refr_now(NULL);
    Serial.println("[ota] update complete, rebooting");
    // ArduinoOTA reboots the device after this callback returns.
}

static void on_error(ota_error_t error) {
    const char *what = "unknown";
    switch (error) {
        case OTA_AUTH_ERROR:    what = "auth failed";    break;
        case OTA_BEGIN_ERROR:   what = "begin failed";   break;
        case OTA_CONNECT_ERROR: what = "connect failed"; break;
        case OTA_RECEIVE_ERROR: what = "receive failed"; break;
        case OTA_END_ERROR:     what = "end failed";     break;
    }
    Serial.printf("[ota] error: %s (%u)\n", what, (unsigned)error);
    // The device keeps running the current firmware — tear the overlay down and
    // let the worker resume so the UI recovers instead of stranding a dead
    // progress bar on screen.
    overlay_destroy();
    worker_resume();
}

void ota_setup() {
    String host = config_ota_hostname();
    String pass = config_ota_password();
    ArduinoOTA.setHostname(host.c_str());
    ArduinoOTA.setPassword(pass.c_str());
    ArduinoOTA.onStart(on_start);
    ArduinoOTA.onEnd(on_end);
    ArduinoOTA.onProgress(on_progress);
    ArduinoOTA.onError(on_error);
    Serial.printf("[ota] configured: host='%s' password='%s'\n",
                  host.c_str(), pass.c_str());
}

void ota_loop() {
    if (!s_started) {
        if (WiFi.status() != WL_CONNECTED) return;  // nothing to advertise yet
        ArduinoOTA.begin();
        s_started = true;
        Serial.printf("[ota] listening as '%s' on ", config_ota_hostname().c_str());
        Serial.println(WiFi.localIP());
    }
    ArduinoOTA.handle();
}
