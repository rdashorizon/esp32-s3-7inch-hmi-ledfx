// ---------------------------------------------------------------------------
// ui_virtuals.cpp — Virtuals tab implementation
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#include "ui_virtuals.h"
#include "ui.h"         // ui_show_status, ui_submit (for the overlay)
#include "worker.h"     // worker_submit, req_*, REQ_*
#include "ui_global.h"  // ui_global_mark_refreshed, ui_global_apply_state_with_pause
#include <Arduino.h>    // millis

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t *s_virt_list = nullptr;
static lv_obj_t *s_virt_spinner = nullptr;
static lv_obj_t *s_virt_error   = nullptr;

// Pause-all switch on the Virtuals toolbar. Owned by this module; its
// checked-state is synced from the server's `paused` flag via the result
// pump, which routes through ui_global_apply_state_with_pause().
static lv_obj_t *s_pause_sw = nullptr;

static VirtualInfo *s_virt = nullptr;  // owned by this module; delete[] on next fetch
static int s_virt_count = 0;

// ---- Per-virtual color modal state (Tier 1) --------------------------------
// All LVGL-thread-only. The modal is a top-layer overlay anchored to the
// Virtuals tab; it builds fresh on each open and destroys on close.
static lv_obj_t *s_color_modal = nullptr;     // top-layer modal root
static int       s_color_modal_idx = -1;     // which row the modal is editing
static uint8_t   s_modal_r = 255, s_modal_g = 200, s_modal_b = 128;
static lv_obj_t *s_modal_wheel;
static lv_obj_t *s_modal_slider_r, *s_modal_slider_g, *s_modal_slider_b;
static lv_obj_t *s_modal_label_r, *s_modal_label_g, *s_modal_label_b;
static lv_obj_t *s_modal_swatch;

// Transient error banner (Tier 1.6). Hidden by default; shown by
// pump_result on a non-200 REQ_SET_VIRTUAL_COLOR (or any other RES_ACTION
// while the Virtuals tab is in front).
static lv_obj_t *s_banner = nullptr;
static lv_timer_t *s_banner_timer = nullptr;
static const uint32_t BANNER_TIMEOUT_MS = 3000;

// ---- Helpers ---------------------------------------------------------------
static void obj_show(lv_obj_t *o, bool show) {
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Map LedFx gradient preset name → representative RGB565 hex. These are
// hand-picked "signature" colors so each gradient reads distinctly in a small
// swatch; they don't try to be physically accurate to the full gradient.
static uint32_t gradient_to_color(const String &name) {
    if (name == "Rainbow")    return 0xff8800;  // orange
    if (name == "Dancefloor") return 0xff00aa;  // magenta
    if (name == "Plasma")     return 0xff22aa;  // pink
    if (name == "Ocean")      return 0x0088ff;  // blue
    if (name == "Viridis")    return 0x44cc66;  // green
    if (name == "Jungle")     return 0x22cc44;  // forest green
    if (name == "Spring")     return 0x66ee88;  // pale green
    if (name == "Winter")     return 0xaaccff;  // icy blue
    if (name == "Frost")      return 0xddeeff;  // pale cyan
    if (name == "Sunset")     return 0xff5522;  // red-orange
    if (name == "Borealis")   return 0x44ffaa;  // aurora green
    if (name == "Rust")       return 0xcc4422;  // rust red
    if (name == "Winamp")     return 0x88ff44;  // lime
    return 0x666666;  // unknown → neutral gray
}

// ---- Event handlers --------------------------------------------------------
static void virt_toggle(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(sw);
    if (idx < 0 || idx >= s_virt_count) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_submit(req_id(REQ_SET_VIRTUAL_ACTIVE, s_virt[idx].id, on));
}

static void virt_randomize(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_virt_count) return;
    ui_submit(req_id(REQ_RANDOMIZE_VIRTUAL, s_virt[idx].id));
    ui_show_status_fmt(false, "Randomizing: %s", s_virt[idx].name.c_str());
}

// Long-press on a row opens a small "what's this" dialog with the virtual's
// id, effect type, effect name, and gradient. Power-user / debug aid.
static void virt_row_long(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (idx < 0 || idx >= s_virt_count) return;
    const VirtualInfo &v = s_virt[idx];
    String body;
    body.reserve(160);
    body += "id: ";
    body += v.id;
    body += "\nactive: ";
    body += v.active ? "yes" : "no";
    body += "\neffect: ";
    body += v.effect_type.isEmpty() ? "(none)" : v.effect_type;
    if (!v.effect_name.isEmpty() && v.effect_name != v.effect_type) {
        body += " (";
        body += v.effect_name;
        body += ")";
    }
    if (!v.gradient.isEmpty()) {
        body += "\ngradient: ";
        body += v.gradient;
    }
    static const char *btns[] = {"Close", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, v.name.c_str(), body.c_str(), btns, true);
    lv_obj_center(mbox);
}

// Forward decls for the per-virtual color modal (Task 1.4 fills in the
// build; Task 1.3 just stubs the entry point so the click handler can wire).
static void open_virtual_color_modal(int idx);

// Tap on a row's gradient swatch → open the per-virtual color picker modal.
// The actual build is in open_virtual_color_modal (Task 1.4 fills it in).
static void swatch_clicked(lv_event_t *e) {
    lv_obj_t *swatch = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(swatch);
    if (idx < 0 || idx >= s_virt_count) return;
    open_virtual_color_modal(idx);
}

static void clear_all_cb(lv_event_t *e) {
    (void)e;
    ui_submit(req_simple(REQ_CLEAR_ALL));
}

static void pause_all_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool paused = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_submit(req_flag(REQ_PAUSE_ALL, paused));
}

// ---- Paint -----------------------------------------------------------------
static void render_virt_list(void) {
    lv_obj_clean(s_virt_list);

    for (int i = 0; i < s_virt_count; i++) {
        lv_obj_t *row = lv_obj_create(s_virt_list);
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        // Long-press the row to inspect; the row owns the user_data index.
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, virt_row_long, LV_EVENT_LONG_PRESSED, NULL);

        // Subtitle: effect type, plus the effect's display name when it adds info.
        String sub;
        if (s_virt[i].effect_type.isEmpty()) {
            sub = "— no effect —";
        } else {
            sub = s_virt[i].effect_type;
            if (!s_virt[i].effect_name.isEmpty() && s_virt[i].effect_name != s_virt[i].effect_type)
                sub += " · " + s_virt[i].effect_name;
        }
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s\n%s", s_virt[i].name.c_str(), sub.c_str());

        // Gradient preview swatch: 20×20 colored square between the name and
        // the randomize button. Falls back to neutral gray when the virtual
        // has no effect or uses a gradient we don't have a color for. Tap
        // → open the per-virtual color picker modal (Task 1.4).
        lv_obj_t *swatch = lv_obj_create(row);
        lv_obj_set_size(swatch, 20, 20);
        lv_obj_set_style_bg_color(swatch,
            lv_color_hex(gradient_to_color(s_virt[i].gradient)), 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(swatch, 2, 0);
        lv_obj_set_user_data(swatch, (void *)(intptr_t)i);
        lv_obj_add_event_cb(swatch, swatch_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *dice = lv_btn_create(row);
        lv_obj_set_size(dice, 50, 50);
        lv_obj_t *dl = lv_label_create(dice);
        lv_label_set_text(dl, LV_SYMBOL_SHUFFLE);
        lv_obj_center(dl);
        lv_obj_set_user_data(dice, (void *)(intptr_t)i);
        lv_obj_add_event_cb(dice, virt_randomize, LV_EVENT_CLICKED, NULL);

        lv_obj_t *sw = lv_switch_create(row);
        if (s_virt[i].active) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_user_data(sw, (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, virt_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (s_virt_count == 0) {
        lv_obj_t *empty = lv_label_create(s_virt_list);
        lv_label_set_text(empty, "No virtuals configured yet.");
    }
    ui_global_mark_refreshed();
    ui_show_status("Virtuals refreshed");
}

// ---- Public API ------------------------------------------------------------
void ui_virtuals_build(lv_obj_t *parent) {
    s_virt_list = lv_obj_create(parent);
    lv_obj_set_size(s_virt_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_virt_list, LV_FLEX_FLOW_COLUMN);

    // Loading spinner + error banner overlaid on the virtuals tab.
    s_virt_spinner = lv_spinner_create(parent, 1000, 60);
    lv_obj_set_size(s_virt_spinner, 56, 56);
    lv_obj_center(s_virt_spinner);
    lv_obj_add_flag(s_virt_spinner, LV_OBJ_FLAG_HIDDEN);
    s_virt_error = lv_label_create(parent);
    lv_obj_set_style_text_color(s_virt_error, lv_color_hex(0xff6666), 0);
    lv_obj_center(s_virt_error);
    lv_obj_add_flag(s_virt_error, LV_OBJ_FLAG_HIDDEN);

    // Tier 1.6: transient action-result banner. Positioned just above the
    // bottom toolbar so it's visible without overlapping the toggle rows.
    s_banner = lv_label_create(parent);
    lv_obj_set_width(s_banner, LV_PCT(100) - 32);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, -76);  // above the toolbar
    lv_obj_set_style_text_color(s_banner, lv_color_hex(0xff8888), 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x331111), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_banner, 8, 0);
    lv_obj_set_style_radius(s_banner, 4, 0);
    lv_label_set_text(s_banner, "");
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    // Clear-all button on virtuals screen
    lv_obj_t *clear_btn = lv_btn_create(parent);
    lv_obj_set_size(clear_btn, 200, 50);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_t *cbl = lv_label_create(clear_btn);
    lv_label_set_text(cbl, "Clear all effects");
    lv_obj_center(cbl);
    lv_obj_add_event_cb(clear_btn, clear_all_cb, LV_EVENT_CLICKED, NULL);

    // Pause-all toggle on virtuals screen (bottom-left toolbar)
    lv_obj_t *pause_wrap = lv_obj_create(parent);
    lv_obj_set_size(pause_wrap, 200, 50);
    lv_obj_align(pause_wrap, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_flex_flow(pause_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pause_wrap, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pl = lv_label_create(pause_wrap);
    lv_label_set_text(pl, "Pause all");
    s_pause_sw = lv_switch_create(pause_wrap);
    lv_obj_add_event_cb(s_pause_sw, pause_all_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void ui_virtuals_request_refresh(void) {
    if (ui_submit(req_simple(REQ_FETCH_VIRTUALS))) {
        obj_show(s_virt_spinner, true);
        obj_show(s_virt_error, false);
        ui_show_status("Refreshing virtuals…");
    }
}

// ---- Per-virtual color modal (Tier 1) --------------------------------------
// Reuses the LVGL colorwheel + RGB-slider pattern from ui_color.cpp but as
// a top-layer modal anchored to the Virtuals tab. Apply sends
// REQ_SET_VIRTUAL_COLOR (PUT /api/virtuals/{id}/effects {type, config:
// {background_color}}). Cancel closes without sending.

static void modal_close(void) {
    if (s_color_modal) {
        lv_obj_del(s_color_modal);
        s_color_modal = nullptr;
    }
    s_color_modal_idx = -1;
}

static void modal_apply_cb(lv_event_t *e) {
    (void)e;
    if (s_color_modal_idx < 0 || s_color_modal_idx >= s_virt_count) return;
    const VirtualInfo &v = s_virt[s_color_modal_idx];
    // If the virtual has no active effect, LedFx will reject the PUT — bail
    // out with a status message rather than sending a request that's
    // guaranteed to fail.
    if (v.effect_type.isEmpty()) {
        ui_show_status("Virtual has no active effect — cannot color", true);
        modal_close();
        return;
    }
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", s_modal_r, s_modal_g, s_modal_b);
    ui_submit(req_set_virtual_color(v.id, v.effect_type, hex));
    modal_close();
}

static void modal_black_cb(lv_event_t *e) {
    (void)e;
    s_modal_r = s_modal_g = s_modal_b = 0;
    if (s_modal_slider_r) lv_slider_set_value(s_modal_slider_r, 0, LV_ANIM_OFF);
    if (s_modal_slider_g) lv_slider_set_value(s_modal_slider_g, 0, LV_ANIM_OFF);
    if (s_modal_slider_b) lv_slider_set_value(s_modal_slider_b, 0, LV_ANIM_OFF);
    if (s_modal_swatch) {
        lv_obj_set_style_bg_color(s_modal_swatch, lv_color_make(0, 0, 0), 0);
    }
    if (s_modal_label_r) lv_label_set_text_fmt(s_modal_label_r, "R   0");
    if (s_modal_label_g) lv_label_set_text_fmt(s_modal_label_g, "G   0");
    if (s_modal_label_b) lv_label_set_text_fmt(s_modal_label_b, "B   0");
}

static void modal_cancel_cb(lv_event_t *e) {
    (void)e;
    modal_close();
}

static void modal_refresh_preview(void) {
    if (s_modal_swatch) {
        lv_obj_set_style_bg_color(s_modal_swatch,
            lv_color_make(s_modal_r, s_modal_g, s_modal_b), 0);
    }
    if (s_modal_label_r) lv_label_set_text_fmt(s_modal_label_r, "R %3d", s_modal_r);
    if (s_modal_label_g) lv_label_set_text_fmt(s_modal_label_g, "G %3d", s_modal_g);
    if (s_modal_label_b) lv_label_set_text_fmt(s_modal_label_b, "B %3d", s_modal_b);
}

static void modal_wheel_cb(lv_event_t *e) {
    lv_color_t c = lv_colorwheel_get_rgb(lv_event_get_target(e));
    s_modal_r = c.ch.red;
    s_modal_g = c.ch.green;
    s_modal_b = c.ch.blue;
    if (s_modal_slider_r) lv_slider_set_value(s_modal_slider_r, s_modal_r, LV_ANIM_OFF);
    if (s_modal_slider_g) lv_slider_set_value(s_modal_slider_g, s_modal_g, LV_ANIM_OFF);
    if (s_modal_slider_b) lv_slider_set_value(s_modal_slider_b, s_modal_b, LV_ANIM_OFF);
    modal_refresh_preview();
}

static void modal_slider_r_cb(lv_event_t *e) {
    s_modal_r = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    modal_refresh_preview();
}
static void modal_slider_g_cb(lv_event_t *e) {
    s_modal_g = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    modal_refresh_preview();
}
static void modal_slider_b_cb(lv_event_t *e) {
    s_modal_b = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    modal_refresh_preview();
}

static void open_virtual_color_modal(int idx) {
    if (s_color_modal) modal_close();  // single-instance guard
    if (idx < 0 || idx >= s_virt_count) return;
    s_color_modal_idx = idx;
    const VirtualInfo &v = s_virt[idx];

    // Seed the modal's RGB from the virtual's current gradient color
    // (same heuristic the row swatch uses). Falls back to warm white.
    if (!v.gradient.isEmpty()) {
        uint32_t hex24 = gradient_to_color(v.gradient);
        s_modal_r = (hex24 >> 16) & 0xff;
        s_modal_g = (hex24 >>  8) & 0xff;
        s_modal_b =  hex24        & 0xff;
    } else {
        s_modal_r = 255; s_modal_g = 200; s_modal_b = 128;
    }

    // Translucent backdrop fills the screen.
    s_color_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_color_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_color_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_color_modal, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_color_modal, 0, 0);
    lv_obj_clear_flag(s_color_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Inner panel (centered) — holds the actual controls. 600×460 fits the
    // 180×180 colorwheel + three sliders + swatch + button row comfortably.
    lv_obj_t *panel = lv_obj_create(s_color_modal);
    lv_obj_set_size(panel, 600, 460);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1a1a22), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);

    // Title row: "Color: <virtual name>" + Cancel button
    lv_obj_t *hdr = lv_obj_create(panel);
    lv_obj_set_size(hdr, LV_PCT(100), 40);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(hdr);
    char t[80];
    snprintf(t, sizeof(t), "Color: %s", v.name.c_str());
    lv_label_set_text(title, t);
    lv_obj_t *x = lv_btn_create(hdr);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x444444), 0);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "Cancel");
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    // Color wheel
    s_modal_wheel = lv_colorwheel_create(panel, true);
    lv_obj_set_size(s_modal_wheel, 180, 180);
    lv_colorwheel_set_rgb(s_modal_wheel,
        lv_color_make(s_modal_r, s_modal_g, s_modal_b));
    lv_obj_add_event_cb(s_modal_wheel, modal_wheel_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // RGB sliders
    auto make_modal_slider = [&](const char *label_text,
                                 lv_obj_t **slider_out,
                                 lv_obj_t **label_out,
                                 lv_event_cb_t cb) {
        lv_obj_t *row = lv_obj_create(panel);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, label_text);
        *label_out = l;
        *slider_out = lv_slider_create(row);
        lv_obj_set_width(*slider_out, 350);
        lv_slider_set_range(*slider_out, 0, 255);
        lv_obj_add_event_cb(*slider_out, cb, LV_EVENT_VALUE_CHANGED, NULL);
        return row;
    };
    make_modal_slider("R", &s_modal_slider_r, &s_modal_label_r, modal_slider_r_cb);
    make_modal_slider("G", &s_modal_slider_g, &s_modal_label_g, modal_slider_g_cb);
    make_modal_slider("B", &s_modal_slider_b, &s_modal_label_b, modal_slider_b_cb);
    lv_slider_set_value(s_modal_slider_r, s_modal_r, LV_ANIM_OFF);
    lv_slider_set_value(s_modal_slider_g, s_modal_g, LV_ANIM_OFF);
    lv_slider_set_value(s_modal_slider_b, s_modal_b, LV_ANIM_OFF);

    // Swatch
    s_modal_swatch = lv_obj_create(panel);
    lv_obj_set_size(s_modal_swatch, LV_PCT(100), 40);
    lv_obj_set_style_border_width(s_modal_swatch, 1, 0);
    lv_obj_set_style_border_color(s_modal_swatch, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_modal_swatch, 4, 0);
    modal_refresh_preview();

    // Buttons: Black (closest-to-off) + Apply
    lv_obj_t *btnrow = lv_obj_create(panel);
    lv_obj_set_size(btnrow, LV_PCT(100), 50);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *black = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(black, lv_color_hex(0x222222), 0);
    lv_obj_set_size(black, 140, 40);
    lv_obj_t *bl = lv_label_create(black);
    lv_label_set_text(bl, "Black");
    lv_obj_center(bl);
    lv_obj_add_event_cb(black, modal_black_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *apply = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(apply, lv_color_hex(0x2266cc), 0);
    lv_obj_set_size(apply, 140, 40);
    lv_obj_t *al = lv_label_create(apply);
    lv_label_set_text(al, LV_SYMBOL_OK "  Apply");
    lv_obj_center(al);
    lv_obj_add_event_cb(apply, modal_apply_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Transient banner (Tier 1.6) -----------------------------------------
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

void ui_virtuals_pump_action_result(int status, const char *msg) {
    if (status != 200 && msg && msg[0]) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  %s", msg);
        banner_show(buf);
    } else if (s_banner) {
        // Success — dismiss any prior error banner immediately.
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        if (s_banner_timer) {
            lv_timer_del(s_banner_timer);
            s_banner_timer = nullptr;
        }
    }
}

void ui_virtuals_pump_result(int status, int count, VirtualInfo *data,
                             const GlobalsState &globals, const char *err) {
    delete[] s_virt;
    s_virt = data;
    s_virt_count = count;
    obj_show(s_virt_spinner, false);
    if (status == 200) {
        obj_show(s_virt_error, false);
        render_virt_list();
        ui_global_apply_state_with_pause(globals, s_pause_sw);
    } else {
        lv_obj_clean(s_virt_list);
        if (s_virt_error)
            lv_label_set_text(s_virt_error,
                (String(LV_SYMBOL_WARNING "  ") + (err && err[0] ? err : "Couldn't reach LedFx")).c_str());
        obj_show(s_virt_error, true);
        ui_show_status(err && err[0] ? err : "Failed to fetch virtuals", true);
    }
}
