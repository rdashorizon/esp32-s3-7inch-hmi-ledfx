// ---------------------------------------------------------------------------
// ui_settings.cpp — on-device settings editor
//
// Edit WiFi/LedFx settings in place (no captive-portal round trip). Opens as
// a separate screen with an on-screen keyboard; Save persists to NVS and
// reboots; Back returns to the tabbed UI root.
//
// Owned by the LVGL thread (core 1). Do not call into the network client.
// ---------------------------------------------------------------------------
#include "ui_settings.h"
#include "ui.h"         // for ui_show_status, ui_root
#include "config.h"     // g_config, config_store, config_url_is_valid
#include "worker.h"     // req_test_connection
#include "ui_theme.h"   // ui_theme_*() palette accessors (Tier 1.6)
#include <Arduino.h>    // ESP.restart, delay
#include <lvgl.h>

extern Config g_config;  // defined in main.cpp

static lv_obj_t *s_settings_scr = nullptr;
static lv_obj_t *s_ta_ssid = nullptr, *s_ta_wpass = nullptr, *s_ta_url = nullptr;
static lv_obj_t *s_ta_user = nullptr, *s_ta_lpass = nullptr, *s_settings_kb = nullptr;

static void settings_ta_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_settings_kb, ta);
        lv_obj_clear_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_kb_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
}

static void settings_close(void) {
    lv_scr_load(ui_root());
    if (s_settings_scr) { lv_obj_del(s_settings_scr); s_settings_scr = nullptr; }
}
static void settings_back_cb(lv_event_t *e) { (void)e; settings_close(); }

static void settings_save_cb(lv_event_t *e) {
    (void)e;
    Config cfg;
    cfg.wifi_ssid  = lv_textarea_get_text(s_ta_ssid);
    cfg.wifi_pass  = lv_textarea_get_text(s_ta_wpass);
    cfg.ledfx_url  = lv_textarea_get_text(s_ta_url);
    cfg.ledfx_user = lv_textarea_get_text(s_ta_user);
    cfg.ledfx_pass = lv_textarea_get_text(s_ta_lpass);
    if (!config_url_is_valid(cfg.ledfx_url)) {
        // Reject without reboot: the user can fix the URL and re-save. The
        // bottom status bar carries the error message.
        ui_show_status("Invalid LedFx URL — must start with http:// or https://", true);
        // Highlight the URL field by giving it focus + a red border.
        lv_obj_add_state(s_ta_url, LV_STATE_FOCUSED);
        // Move the keyboard back over the URL field if it was hidden.
        lv_keyboard_set_textarea(s_settings_kb, s_ta_url);
        lv_obj_clear_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    config_store.save(cfg);
    ui_show_status("Settings saved — rebooting…");
    lv_refr_now(NULL);
    delay(600);
    ESP.restart();
}

static lv_obj_t *settings_field(lv_obj_t *parent, const char *label,
                                const String &value, bool password) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, value.c_str());
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_ALL, NULL);
    return ta;
}

// Test the URL/credentials over the current WiFi (no save, no reboot).
static void settings_test_cb(lv_event_t *e) {
    (void)e;
    if (!config_url_is_valid(lv_textarea_get_text(s_ta_url))) {
        ui_show_status("Invalid LedFx URL — must start with http:// or https://", true);
        return;
    }
    ui_submit(req_test_connection(
        lv_textarea_get_text(s_ta_url),
        lv_textarea_get_text(s_ta_user),
        lv_textarea_get_text(s_ta_lpass)));
    ui_show_status("Testing connection…");
}

static void settings_open(void) {
    s_settings_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_scr,
        lv_color_hex(ui_theme_root_bg()), 0);
    lv_obj_clear_flag(s_settings_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header: Back / title / Test / Save
    lv_obj_t *hdr = lv_obj_create(s_settings_scr);
    lv_obj_set_size(hdr, LV_PCT(100), 56);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " Back"); lv_obj_center(bl);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ttl = lv_label_create(hdr); lv_label_set_text(ttl, "Settings");
    lv_obj_t *test = lv_btn_create(hdr);
    lv_obj_set_style_bg_color(test,
        lv_color_hex(ui_theme_panel_alt()), 0);
    lv_obj_t *tl = lv_label_create(test); lv_label_set_text(tl, LV_SYMBOL_REFRESH " Test"); lv_obj_center(tl);
    lv_obj_add_event_cb(test, settings_test_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = lv_btn_create(hdr);
    lv_obj_set_style_bg_color(save,
        lv_color_hex(ui_theme_accent_rgb565()), 0);
    lv_obj_t *svl = lv_label_create(save); lv_label_set_text(svl, LV_SYMBOL_SAVE " Save"); lv_obj_center(svl);
    lv_obj_add_event_cb(save, settings_save_cb, LV_EVENT_CLICKED, NULL);

    // Scrollable form (sits above the keyboard when it appears)
    lv_obj_t *form = lv_obj_create(s_settings_scr);
    lv_obj_set_size(form, LV_PCT(100), 400);
    lv_obj_align(form, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    s_ta_ssid  = settings_field(form, "WiFi SSID", g_config.wifi_ssid, false);
    s_ta_wpass = settings_field(form, "WiFi password", g_config.wifi_pass, true);
    s_ta_url   = settings_field(form, "LedFx URL", g_config.ledfx_url, false);
    s_ta_user  = settings_field(form, "LedFx user (optional)", g_config.ledfx_user, false);
    s_ta_lpass = settings_field(form, "LedFx password (optional)", g_config.ledfx_pass, true);

    // On-screen keyboard, hidden until a field is focused.
    s_settings_kb = lv_keyboard_create(s_settings_scr);
    lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_settings_kb, settings_kb_event, LV_EVENT_ALL, NULL);

    lv_scr_load(s_settings_scr);
}

void ui_settings_open(void) {
    settings_open();
}

void ui_settings_register_button(lv_obj_t *btn) {
    lv_obj_add_event_cb(btn, [](lv_event_t *e) { (void)e; settings_open(); },
                        LV_EVENT_CLICKED, NULL);
}
