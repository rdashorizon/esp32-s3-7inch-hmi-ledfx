// ---------------------------------------------------------------------------
// ui_virtuals_color.cpp — per-virtual color picker modal implementation
//
// Extracted from ui_virtuals.cpp. See ui_virtuals_color.h for the public
// API and the threading contract.
//
// Owned by the LVGL thread (core 1).
// ---------------------------------------------------------------------------
#include "ui_virtuals_color.h"
#include "ui.h"          // ui_submit, ui_show_status
#include "ui_common.h"   // UiColorPicker
#include "worker.h"      // req_set_virtual_color, REQ_SET_VIRTUAL_COLOR
#include "config.h"      // LedColor, load/save_last_virt_color
#include "ui_theme.h"    // ui_theme_*() palette accessors
#include <Arduino.h>     // millis

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t     *s_color_modal = nullptr;   // top-layer modal root
static int           s_color_modal_idx = -1;    // which row the modal is editing
static UiColorPicker s_picker;

// Snapshot of the virtual being edited, taken when the modal opens.
//
// These are deliberately owned *copies*, not a pointer into the caller's
// VirtualInfo: that array lives in ui_virtuals and is delete[]'d and replaced
// on every fetch, including the 30 s auto-refresh and the refresh that
// follows every action. A refresh landing while the modal is open would leave
// any retained pointer dangling, and Apply would read freed memory.
static String s_target_id;
static String s_target_effect;

// Default seed color (warm white) shared with the global Color tab, so a
// brand-new device behaves the same whichever picker the user opens first.
static const LedColor DEFAULT_SEED = {255, 200, 128};

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

// The row index of the most recent successful color apply. The next time
// render_virt_list() draws that row it gets an accent border for ~1.5 s as
// confirmation. -1 means "no recent apply".
static int      s_last_set_idx = -1;
static uint32_t s_last_set_ms = 0;
static const uint32_t FLASH_TIMEOUT_MS = 1500;

// ---- Close -----------------------------------------------------------------
static void modal_close(void) {
    if (s_color_modal) {
        lv_obj_del(s_color_modal);
        s_color_modal = nullptr;
        // The picker's widgets were children of the modal and are gone now.
        s_picker.reset();
    }
    s_color_modal_idx = -1;
    s_target_id = "";
    s_target_effect = "";
}

// ---- Event handlers --------------------------------------------------------
static void modal_apply_cb(lv_event_t *e) {
    (void)e;
    if (s_color_modal_idx < 0) return;
    // If the virtual had no active effect when the modal opened, LedFx will
    // reject the PUT — bail out rather than sending a doomed request.
    if (s_target_effect.isEmpty()) {
        ui_show_status("Virtual has no active effect — cannot color", true);
        modal_close();
        return;
    }
    LedColor c = s_picker.rgb();
    // Record for the flash-on-next-render. Stale entries expire after
    // FLASH_TIMEOUT_MS — checked at render time.
    s_last_set_idx = s_color_modal_idx;
    s_last_set_ms = millis();
    // Persist the picked color so the modal reopens at this RGB next time.
    // Single global value — see plan Risks for why we don't track per-virtual
    // history.
    config_store.save_last_virt_color(c);
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.r, c.g, c.b);
    ui_submit(req_set_virtual_color(s_target_id, s_target_effect, hex));
    modal_close();
}

static void modal_black_cb(lv_event_t *e) {
    (void)e;
    s_picker.set_rgb({0, 0, 0});
}

static void modal_cancel_cb(lv_event_t *e) {
    (void)e;
    modal_close();
}

// ---- Open ------------------------------------------------------------------
void ui_virtuals_color_open(int idx, const VirtualInfo &v) {
    if (s_color_modal) modal_close();  // single-instance guard
    if (idx < 0) return;
    s_color_modal_idx = idx;
    // Copy everything Apply will need; see the note on s_target_id above.
    s_target_id     = v.id;
    s_target_effect = v.effect_type;

    // Seed the modal's RGB. Preference order:
    //   1. The user's last picked color (across reboots), if it differs from
    //      the warm-white default — that means they have actually picked
    //      something via this modal at least once.
    //   2. The virtual's current gradient color (same heuristic the row
    //      swatch uses).
    //   3. Warm white as a final fallback.
    LedColor saved = config_store.load_last_virt_color();
    bool saved_is_default = (saved.r == DEFAULT_SEED.r &&
                             saved.g == DEFAULT_SEED.g &&
                             saved.b == DEFAULT_SEED.b);
    LedColor seed = DEFAULT_SEED;
    if (!saved_is_default) {
        seed = saved;
    } else if (!v.gradient.isEmpty()) {
        uint32_t hex24 = ui_virtuals_color_gradient_to_rgb565(v.gradient);
        seed = { (uint8_t)((hex24 >> 16) & 0xff),
                 (uint8_t)((hex24 >>  8) & 0xff),
                 (uint8_t)( hex24        & 0xff) };
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
    lv_obj_set_style_bg_color(panel, lv_color_hex(ui_theme_panel_bg()), 0);
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
    lv_obj_set_style_bg_color(x, lv_color_hex(ui_theme_panel_alt()), 0);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "Cancel");
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    // Colorwheel + RGB sliders + swatch. No on_change hook: the modal is
    // explicit-commit (Apply), unlike the global Color tab's live preview.
    s_picker.set_rgb(seed);
    const UiColorPicker::Metrics m = { 180, 350, 36, 40 };
    s_picker.build(panel, m, nullptr, nullptr);

    // Buttons: Black (closest-to-off) + Apply
    lv_obj_t *btnrow = lv_obj_create(panel);
    lv_obj_set_size(btnrow, LV_PCT(100), 50);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *black = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(black, lv_color_hex(ui_theme_panel_alt()), 0);
    lv_obj_set_size(black, 140, 40);
    lv_obj_t *bl = lv_label_create(black);
    lv_label_set_text(bl, "Black");
    lv_obj_center(bl);
    lv_obj_add_event_cb(black, modal_black_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *apply = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(apply, lv_color_hex(ui_theme_accent_rgb565()), 0);
    lv_obj_set_size(apply, 140, 40);
    lv_obj_t *al = lv_label_create(apply);
    lv_label_set_text(al, LV_SYMBOL_OK "  Apply");
    lv_obj_center(al);
    lv_obj_add_event_cb(apply, modal_apply_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Getters for the parent module ---------------------------------------
bool ui_virtuals_color_row_is_recent(int idx) {
    if (s_last_set_idx < 0) return false;
    if (idx != s_last_set_idx) return false;
    return (millis() - s_last_set_ms) < FLASH_TIMEOUT_MS;
}
