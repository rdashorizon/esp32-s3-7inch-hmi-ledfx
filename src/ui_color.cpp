// ---------------------------------------------------------------------------
// ui_color.cpp — Color picker tab implementation
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
//
// Layout (flex column):
//   - Colorwheel + R/G/B slider rows + live-preview swatch (UiColorPicker)
//   - Transient error banner (UiBanner)
//   - Bottom row: Apply button + Black button
// ---------------------------------------------------------------------------
#include "ui_color.h"
#include "ui.h"            // ui_submit, ui_show_status
#include "ui_common.h"     // UiColorPicker, UiBanner
#include "worker.h"        // req_payload, REQ_APPLY_GLOBAL
#include "config.h"        // LedColor, load/save_last_color
#include "ui_theme.h"      // ui_theme_*() palette accessors
#include <Arduino.h>       // millis
#include <ArduinoJson.h>   // StaticJsonDocument

// ---- Module state (LVGL-thread-only) --------------------------------------
static UiColorPicker s_picker;
static UiBanner      s_banner;
static lv_obj_t     *s_apply_btn = nullptr;
static lv_obj_t     *s_black_btn = nullptr;

// Throttle the live-apply while the user is dragging the wheel or a slider.
static uint32_t s_last_apply_ms = 0;
static const uint32_t LIVE_APPLY_MIN_INTERVAL_MS = 250;

// Throttled apply during drag — at most one PUT per 250 ms. The Apply and
// Black buttons call ui_color_apply_now() directly, unthrottled.
static void on_picker_change(void *) {
    uint32_t now = millis();
    if (now - s_last_apply_ms < LIVE_APPLY_MIN_INTERVAL_MS) return;
    s_last_apply_ms = now;
    ui_color_apply_now();
}

// ---- Public API ------------------------------------------------------------
void ui_color_apply_now(void) {
    LedColor c = s_picker.rgb();
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    // LedFx's apply_global uses `background_color` (a CSS-style hex string),
    // not a nested object — confirmed against LedFx/docs/apis/global.md.
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.r, c.g, c.b);
    doc["background_color"] = hex;
    String body;
    serializeJson(doc, body);
    ui_submit(req_payload(REQ_APPLY_GLOBAL, body));
    // Persist the picked color so the next boot returns to the same color.
    config_store.save_last_color(c);
}

void ui_color_pump_result(int status, const char *msg) {
    // On a non-200 apply, surface the failure on the Color tab itself — the
    // bottom status label is easy to miss while the user is staring at the
    // wheel. msg is the action result text; we only need to know it failed.
    if (status != 200 && msg && msg[0]) {
        char banner[96];
        snprintf(banner, sizeof(banner), LV_SYMBOL_WARNING "  Apply failed: %s", msg);
        s_banner.show(banner);
    } else {
        s_banner.hide();   // success — drop any stale error immediately
    }
}

// ---- Build -----------------------------------------------------------------
void ui_color_build(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 16, 0);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Restore the user's last-picked color (or the warm-white default on first
    // boot) before building, so the wheel starts in the right position and the
    // swatch shows the saved color immediately.
    s_picker.set_rgb(config_store.load_last_color());
    const UiColorPicker::Metrics m = { 220, 400, 50, 60 };
    s_picker.build(parent, m, on_picker_change, nullptr);

    // Transient error banner — hidden until an apply fails.
    s_banner.build(parent, LV_PCT(100));

    // ---- Apply + Black buttons ----
    lv_obj_t *btnrow = lv_obj_create(parent);
    lv_obj_set_size(btnrow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_apply_btn = lv_btn_create(btnrow);
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
    lv_obj_set_size(s_black_btn, 200, 50);
    lv_obj_t *bl = lv_label_create(s_black_btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER "  Black");
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_black_btn,
        [](lv_event_t *e) {
            (void)e;
            s_picker.set_rgb({0, 0, 0});
            ui_color_apply_now();
        },
        LV_EVENT_CLICKED, NULL);

    ui_color_apply_theme();   // paint the buttons in the current accent
}

// ---- Theme support ---------------------------------------------------------
void ui_color_apply_theme(void) {
    s_picker.apply_theme();
    s_banner.apply_theme();
    if (s_apply_btn) {
        lv_obj_set_style_bg_color(s_apply_btn,
            lv_color_hex(ui_theme_accent_rgb565()), 0);
    }
    if (s_black_btn) {
        lv_obj_set_style_bg_color(s_black_btn,
            lv_color_hex(ui_theme_panel_alt()), 0);
    }
}

namespace ui_color {
    void apply_theme() { ui_color_apply_theme(); }
}
