// ---------------------------------------------------------------------------
// ui_virtuals_color.cpp — per-virtual color picker modal implementation
//
// Extracted from ui_virtuals.cpp. See ui_virtuals_color.h for the public
// API and the threading contract.
//
// Owned by the LVGL thread (core 1).
// ---------------------------------------------------------------------------
#include "ui_virtuals_color.h"
#include "ui.h"         // ui_submit, ui_show_status
#include "ui_virtuals.h" // gradient_to_color() — still lives in the parent
#include "worker.h"     // req_set_virtual_color, REQ_SET_VIRTUAL_COLOR
#include "config.h"     // LedColor, load/save_last_virt_color
#include <Arduino.h>    // millis

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t *s_color_modal = nullptr;     // top-layer modal root
static int       s_color_modal_idx = -1;     // which row the modal is editing
static uint8_t   s_modal_r = 255, s_modal_g = 200, s_modal_b = 128;
static lv_obj_t *s_modal_wheel;
static lv_obj_t *s_modal_slider_r, *s_modal_slider_g, *s_modal_slider_b;
static lv_obj_t *s_modal_label_r, *s_modal_label_g, *s_modal_label_b;
static lv_obj_t *s_modal_swatch;

// Map LedFx gradient preset name → representative RGB565 hex. These are
// hand-picked "signature" colors so each gradient reads distinctly in a
// small swatch; they don't try to be physically accurate to the full gradient.
uint32_t ui_virtuals_color_gradient_to_rgb565(const String &name) {
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

// Tier 2.1: the row index of the most recent successful color apply.
// The next time render_virt_list() draws that row, it gets a green border
// for ~1.5 s as confirmation. -1 means "no recent apply". The parent
// module reads this via ui_virtuals_color_last_set_idx() on each render.
static int  s_last_set_idx = -1;
static uint32_t s_last_set_ms = 0;
static const uint32_t FLASH_TIMEOUT_MS = 1500;

// Forward declaration — defined below in modal_build.
static void modal_close(void);

// ---- Event handlers --------------------------------------------------------
static void modal_apply_cb(lv_event_t *e) {
    (void)e;
    if (s_color_modal_idx < 0) return;
    const VirtualInfo &v = *(const VirtualInfo *)lv_obj_get_user_data(s_modal_wheel);
    // If the virtual has no active effect, LedFx will reject the PUT — bail
    // out with a status message rather than sending a request that's
    // guaranteed to fail.
    if (v.effect_type.isEmpty()) {
        ui_show_status("Virtual has no active effect — cannot color", true);
        modal_close();
        return;
    }
    // Record for the flash-on-next-render (Tier 2.1). Stale entries expire
    // after FLASH_TIMEOUT_MS — checked at render time.
    s_last_set_idx = s_color_modal_idx;
    s_last_set_ms = millis();
    // Persist the picked color so the modal reopens at this RGB next time
    // (Tier 2.2). Single global value — see plan Risks for why we don't
    // track per-virtual history.
    config_store.save_last_virt_color({s_modal_r, s_modal_g, s_modal_b});
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

// ---- Close -----------------------------------------------------------------
static void modal_close(void) {
    if (s_color_modal) {
        lv_obj_del(s_color_modal);
        s_color_modal = nullptr;
    }
    s_color_modal_idx = -1;
}

// ---- Open ------------------------------------------------------------------
void ui_virtuals_color_open(int idx, const VirtualInfo &v) {
    (void)v;  // referenced via modal callbacks that pull from user_data
    if (s_color_modal) modal_close();  // single-instance guard
    if (idx < 0) return;
    s_color_modal_idx = idx;
    const VirtualInfo &vv = v;  // alias for readability below

    // Seed the modal's RGB. Preference order (Tier 2.2):
    //   1. The user's last picked color (across reboots), if it differs
    //      from the warm-white default — that means they have actually
    //      picked something via this modal at least once.
    //   2. The virtual's current gradient color (same heuristic the row
    //      swatch uses).
    //   3. Warm white (255, 200, 128) as a final fallback.
    LedColor saved = config_store.load_last_virt_color();
    bool saved_is_default = (saved.r == 255 && saved.g == 200 && saved.b == 128);
    if (!saved_is_default) {
        s_modal_r = saved.r;
        s_modal_g = saved.g;
        s_modal_b = saved.b;
    } else if (!vv.gradient.isEmpty()) {
        uint32_t hex24 = ui_virtuals_color_gradient_to_rgb565(vv.gradient);
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
    // Stash the VirtualInfo pointer on the wheel so the apply callback can
    // read the id + effect_type without re-entering the parent module.
    // (The wheel is the only LVGL object whose user_data we can read from
    // every callback that fires after build, since lv_obj_get_user_data
    // on the backdrop itself works too but is less obvious.)
    // (assignment below after the wheel is built)

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
    snprintf(t, sizeof(t), "Color: %s", vv.name.c_str());
    lv_label_set_text(title, t);
    lv_obj_t *x = lv_btn_create(hdr);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x444444), 0);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "Cancel");
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    // Color wheel — store the VirtualInfo* on the wheel so modal_apply_cb
    // (which only gets the wheel's user_data in scope via lv_event_get_target
    // chain, but we're using a free function not the event system to reach
    // v) can dereference it.
    s_modal_wheel = lv_colorwheel_create(panel, true);
    lv_obj_set_size(s_modal_wheel, 180, 180);
    lv_colorwheel_set_rgb(s_modal_wheel,
        lv_color_make(s_modal_r, s_modal_g, s_modal_b));
    lv_obj_set_user_data(s_modal_wheel, (void *)&vv);
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

// ---- Getters for the parent module ---------------------------------------
int ui_virtuals_color_last_set_idx(void) { return s_last_set_idx; }

bool ui_virtuals_color_row_is_recent(int idx) {
    if (s_last_set_idx < 0) return false;
    if (idx != s_last_set_idx) return false;
    return (millis() - s_last_set_ms) < FLASH_TIMEOUT_MS;
}
