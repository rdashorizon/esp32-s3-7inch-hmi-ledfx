// ---------------------------------------------------------------------------
// ui_color.cpp — Color picker tab implementation
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
//
// Layout (flex column):
//   - Colorwheel (220×220)
//   - R / G / B slider rows (label + slider)
//   - Live-preview swatch
//   - Bottom row: Apply button + Black button (Tier 2 adds the Black btn)
//   - Transient error banner (Tier 2)
// ---------------------------------------------------------------------------
#include "ui_color.h"
#include "ui.h"            // ui_submit, ui_show_status
#include "worker.h"        // req_payload, REQ_APPLY_GLOBAL
#include "config.h"        // LedColor, load/save_last_color (Tier 1.5)
#include <Arduino.h>       // millis
#include <ArduinoJson.h>   // StaticJsonDocument (Tier 1.4)
#include <stdarg.h>        // va_list (banner; Tier 2)

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t *s_colorwheel;
static lv_obj_t *s_slider_r, *s_slider_g, *s_slider_b;
static lv_obj_t *s_label_r, *s_label_g, *s_label_b;
static lv_obj_t *s_swatch;
static lv_obj_t *s_apply_btn;
static lv_obj_t *s_black_btn;
// Transient error banner (Tier 2.1). Hidden by default; shown by
// pump_result on a non-200 apply, auto-hidden after BANNER_TIMEOUT_MS.
static lv_obj_t *s_banner = nullptr;
static lv_timer_t *s_banner_timer = nullptr;
static const uint32_t BANNER_TIMEOUT_MS = 3000;

// Current color in 0..255 per channel. Defaults are read from NVS on build
// so the device restores the user's last pick; if no pick has been saved,
// load_last_color() returns warm white (255, 200, 128).
static uint8_t s_r = 255, s_g = 200, s_b = 128;
static uint32_t s_last_apply_ms = 0;  // throttle live-apply during drag

// Forward decls
static void apply_preview_color(void);
static void maybe_live_apply(void);

// ---- Event handlers --------------------------------------------------------
static void colorwheel_cb(lv_event_t *e) {
    lv_obj_t *cw = lv_event_get_target(e);
    lv_color_t c = lv_colorwheel_get_rgb(cw);
    s_r = c.ch.red;
    s_g = c.ch.green;
    s_b = c.ch.blue;
    // Keep the RGB sliders in sync with the wheel.
    if (s_slider_r) lv_slider_set_value(s_slider_r, s_r, LV_ANIM_OFF);
    if (s_slider_g) lv_slider_set_value(s_slider_g, s_g, LV_ANIM_OFF);
    if (s_slider_b) lv_slider_set_value(s_slider_b, s_b, LV_ANIM_OFF);
    apply_preview_color();
    maybe_live_apply();
}

static void slider_r_cb(lv_event_t *e) {
    s_r = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    apply_preview_color();
    maybe_live_apply();
}
static void slider_g_cb(lv_event_t *e) {
    s_g = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    apply_preview_color();
    maybe_live_apply();
}
static void slider_b_cb(lv_event_t *e) {
    s_b = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    apply_preview_color();
    maybe_live_apply();
}

// ---- Live preview ----------------------------------------------------------
static void apply_preview_color(void) {
    if (s_swatch) {
        lv_obj_set_style_bg_color(s_swatch, lv_color_make(s_r, s_g, s_b), 0);
    }
    if (s_label_r) lv_label_set_text_fmt(s_label_r, "R %3d", s_r);
    if (s_label_g) lv_label_set_text_fmt(s_label_g, "G %3d", s_g);
    if (s_label_b) lv_label_set_text_fmt(s_label_b, "B %3d", s_b);
    // Keep the wheel in sync when the sliders move (the wheel drives the
    // sliders on its own LV_EVENT_VALUE_CHANGED — we don't echo here to
    // avoid an event storm).
}

// Throttled apply during drag — at most once per 250 ms while the user is
// actively moving the controls. The Apply button + final release path both
// call ui_color_apply_now() unsuppressed.
static void maybe_live_apply(void) {
    uint32_t now = millis();
    if (now - s_last_apply_ms < 250) return;
    s_last_apply_ms = now;
    ui_color_apply_now();
}

// ---- Banner (Tier 2.1) -----------------------------------------------------
static void banner_hide_cb(lv_timer_t *t) {
    (void)t;
    if (s_banner) lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    if (s_banner_timer) {
        lv_timer_del(s_banner_timer);
        s_banner_timer = nullptr;
    }
}

static void banner_show(const char *msg) {
    if (!s_banner) return;
    lv_label_set_text(s_banner, msg);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    if (s_banner_timer) lv_timer_del(s_banner_timer);
    s_banner_timer = lv_timer_create(banner_hide_cb, BANNER_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
}

// ---- Public API ------------------------------------------------------------
void ui_color_apply_now(void) {
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    // LedFx's apply_global uses `background_color` (a CSS-style hex string),
    // not a nested object — confirmed against LedFx/docs/apis/global.md.
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", s_r, s_g, s_b);
    doc["background_color"] = hex;
    String body;
    serializeJson(doc, body);
    ui_submit(req_payload(REQ_APPLY_GLOBAL, body));
    // Persist the picked color so the next boot returns to the same color.
    config_store.save_last_color({s_r, s_g, s_b});
}

void ui_color_pump_result(int status, const char *msg) {
    // On a non-200 apply, surface the failure on the Color tab itself —
    // the bottom status label is easy to miss while the user is staring at
    // the wheel. msg is the action result ("Scene activated", "Gradient set",
    // etc) — for our purposes we just need to know it failed.
    if (status != 200 && msg && msg[0]) {
        char banner[96];
        snprintf(banner, sizeof(banner), LV_SYMBOL_WARNING "  Apply failed: %s", msg);
        banner_show(banner);
    } else if (s_banner) {
        // Success — dismiss any prior error banner immediately.
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        if (s_banner_timer) {
            lv_timer_del(s_banner_timer);
            s_banner_timer = nullptr;
        }
    }
}

// ---- Build -----------------------------------------------------------------
void ui_color_build(lv_obj_t *parent) {
    // Restore the user's last-picked color (or warm-white default on first
    // boot). Done before widget creation so the colorwheel starts in the
    // right position and the swatch shows the saved color immediately.
    LedColor saved = config_store.load_last_color();
    s_r = saved.r;
    s_g = saved.g;
    s_b = saved.b;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 16, 0);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // ---- Colorwheel ----
    // knob_recolor=true: the central knob is tinted with the current color,
    // which doubles as a small inline preview of the picked color.
    s_colorwheel = lv_colorwheel_create(parent, true);
    lv_obj_set_size(s_colorwheel, 220, 220);
    lv_colorwheel_set_rgb(s_colorwheel, lv_color_make(s_r, s_g, s_b));
    lv_obj_add_event_cb(s_colorwheel, colorwheel_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ---- RGB sliders ----
    auto make_slider = [&](const char *label_text, lv_obj_t **slider_out,
                           lv_obj_t **label_out, lv_event_cb_t cb) {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), 50);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, label_text);
        *label_out = l;  // apply_preview_color updates this with "R NNN"
        *slider_out = lv_slider_create(row);
        lv_obj_set_width(*slider_out, 400);
        lv_slider_set_range(*slider_out, 0, 255);
        lv_obj_add_event_cb(*slider_out, cb, LV_EVENT_VALUE_CHANGED, NULL);
        return row;
    };
    make_slider("R", &s_slider_r, &s_label_r, slider_r_cb);
    make_slider("G", &s_slider_g, &s_label_g, slider_g_cb);
    make_slider("B", &s_slider_b, &s_label_b, slider_b_cb);
    lv_slider_set_value(s_slider_r, s_r, LV_ANIM_OFF);
    lv_slider_set_value(s_slider_g, s_g, LV_ANIM_OFF);
    lv_slider_set_value(s_slider_b, s_b, LV_ANIM_OFF);

    // ---- Live-preview swatch ----
    s_swatch = lv_obj_create(parent);
    lv_obj_set_size(s_swatch, LV_PCT(100), 60);
    lv_obj_set_style_border_width(s_swatch, 1, 0);
    lv_obj_set_style_border_color(s_swatch, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_swatch, 4, 0);
    apply_preview_color();

    // ---- Error banner (Tier 2.1) ----
    // Hidden by default; surfaces apply failures briefly so the user notices
    // without the bottom status bar stealing focus from the wheel.
    s_banner = lv_label_create(parent);
    lv_obj_set_width(s_banner, LV_PCT(100));
    lv_obj_set_style_text_color(s_banner, lv_color_hex(0xff8888), 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x331111), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_banner, 8, 0);
    lv_obj_set_style_radius(s_banner, 4, 0);
    lv_label_set_text(s_banner, "");
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    // ---- Apply + Black buttons (Tier 2.2) ----
    lv_obj_t *btnrow = lv_obj_create(parent);
    lv_obj_set_size(btnrow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_apply_btn = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(s_apply_btn, lv_color_hex(0x2266cc), 0);
    lv_obj_set_size(s_apply_btn, 200, 50);
    lv_obj_t *al = lv_label_create(s_apply_btn);
    lv_label_set_text(al, LV_SYMBOL_OK "  Apply");
    lv_obj_center(al);
    lv_obj_add_event_cb(s_apply_btn,
        [](lv_event_t *e) { (void)e; ui_color_apply_now(); },
        LV_EVENT_CLICKED, NULL);

    // Black button: closest thing to "off" — see Risks in the plan. Sends
    // background_color="#000000" which LedFx treats as a known dark state.
    s_black_btn = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(s_black_btn, lv_color_hex(0x222222), 0);
    lv_obj_set_size(s_black_btn, 200, 50);
    lv_obj_t *bl = lv_label_create(s_black_btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER "  Black");
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_black_btn,
        [](lv_event_t *e) {
            (void)e;
            s_r = s_g = s_b = 0;
            if (s_slider_r) lv_slider_set_value(s_slider_r, 0, LV_ANIM_OFF);
            if (s_slider_g) lv_slider_set_value(s_slider_g, 0, LV_ANIM_OFF);
            if (s_slider_b) lv_slider_set_value(s_slider_b, 0, LV_ANIM_OFF);
            apply_preview_color();
            ui_color_apply_now();
        },
        LV_EVENT_CLICKED, NULL);
}
