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
        // has no effect or uses a gradient we don't have a color for.
        lv_obj_t *swatch = lv_obj_create(row);
        lv_obj_set_size(swatch, 20, 20);
        lv_obj_set_style_bg_color(swatch,
            lv_color_hex(gradient_to_color(s_virt[i].gradient)), 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(swatch, 2, 0);

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
