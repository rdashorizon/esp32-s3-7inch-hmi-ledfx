// ---------------------------------------------------------------------------
// ui_overlay.cpp — top-layer overlay widgets (slow-network warning)
//
// Owned by the LVGL thread (core 1).
// ---------------------------------------------------------------------------
#include "ui_overlay.h"
#include <lvgl.h>
#include <Arduino.h>  // millis

// How long after the last submit (with no progress) before the overlay shows.
static const uint32_t SLOW_THRESHOLD_MS = 1500;

// State. All LVGL-thread-only.
static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_overlay_label = nullptr;
static bool      s_visible = false;
static uint32_t  s_last_submit_ms = 0;  // 0 = no submit since boot

// ---- Helpers ---------------------------------------------------------------
static void overlay_show(void) {
    if (s_visible) return;
    if (!s_overlay) return;
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
}

static void overlay_hide(void) {
    if (!s_visible) return;
    if (!s_overlay) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

static void build_overlay(void) {
    // Translucent dim over the entire screen with a centered label.
    // Colors here are intentionally theme-neutral (black backdrop + white
    // label) — the overlay is a veil meant to read on top of whatever
    // tab/accent is currently in front. Theme-aware re-styling would
    // break that contract.
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);  // 50% black overlay
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Click-through: don't steal input from the active tab.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);  // safe — see below
    // Actually, we want it to NOT absorb clicks. LVGL 8.x: clear the clickable
    // flag and let events bubble up. The tab beneath still receives them.
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    s_overlay_label = lv_label_create(s_overlay);
    lv_label_set_text(s_overlay_label, LV_SYMBOL_REFRESH "  Still working…");
    lv_obj_set_style_text_color(s_overlay_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_overlay_label);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ---- Periodic check --------------------------------------------------------
// Escalate to the overlay if the last submit was more than SLOW_THRESHOLD_MS
// ago with no progress. Runs every 250 ms.
static void slow_tick_cb(lv_timer_t *t) {
    (void)t;
    if (s_last_submit_ms == 0) {
        overlay_hide();
        return;
    }
    uint32_t idle = millis() - s_last_submit_ms;
    if (idle > SLOW_THRESHOLD_MS) overlay_show();
    else                          overlay_hide();
}

// ---- Public API ------------------------------------------------------------
void ui_overlay_mark_submit(void) {
    s_last_submit_ms = millis();
    // Don't show immediately — let the threshold tick catch up. This also
    // hides a previously-shown overlay if the user fired off another quick
    // request before the threshold elapsed.
    overlay_hide();
}

void ui_overlay_mark_progress(void) {
    s_last_submit_ms = 0;  // any progress = no outstanding "slow" request
    overlay_hide();
}

void ui_overlay_install(void) {
    build_overlay();
    lv_timer_create(slow_tick_cb, 250, NULL);
}
