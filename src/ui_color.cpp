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
#include "config.h"        // RGBColor, load/save_last_color (Tier 1.5)
#include <Arduino.h>       // millis
#include <ArduinoJson.h>   // StaticJsonDocument (Tier 1.4)
#include <stdarg.h>        // va_list (banner; Tier 2)

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t *s_colorwheel;
static lv_obj_t *s_slider_r, *s_slider_g, *s_slider_b;
static lv_obj_t *s_label_r, *s_label_g, *s_label_b;
static lv_obj_t *s_swatch;
static lv_obj_t *s_apply_btn;
static lv_obj_t *s_banner = nullptr;  // hidden by default; shown by pump_result

// Current color in 0..255 per channel. Defaults to a warm white so the
// device starts in a non-dark state and the LED strip is visible.
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
}

void ui_color_pump_result(int status, const char *msg) {
    // Tier 2.1 will surface this as a transient banner on the tab. For now
    // we just log — the bottom status label (painted by the result pump in
    // ui.cpp on RES_ACTION) already shows success/failure to the user.
    (void)status;
    (void)msg;
}

// ---- Build -----------------------------------------------------------------
void ui_color_build(lv_obj_t *parent) {
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

    // ---- Apply button row ----
    // Tier 2.2 adds the Black button next to Apply.
    s_apply_btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(s_apply_btn, lv_color_hex(0x2266cc), 0);
    lv_obj_t *al = lv_label_create(s_apply_btn);
    lv_label_set_text(al, LV_SYMBOL_OK "  Apply");
    lv_obj_center(al);
    lv_obj_add_event_cb(s_apply_btn,
        [](lv_event_t *e) { (void)e; ui_color_apply_now(); },
        LV_EVENT_CLICKED, NULL);
}
