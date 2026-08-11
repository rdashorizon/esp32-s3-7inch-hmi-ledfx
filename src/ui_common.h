// ---------------------------------------------------------------------------
// ui_common.h — widgets shared by more than one UI module
//
// Everything here was duplicated across two or more of ui_scenes / ui_virtuals
// / ui_color / ui_virtuals_color before being hoisted. Nothing in this file
// knows about LedFx or the network — it is pure LVGL composition.
//
// Owned by the LVGL thread (core 1), like every other ui_* module.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include <Arduino.h>
#include "config.h"   // LedColor

// Show/hide an optional widget. Null-safe so callers don't have to guard
// widgets that may not have been built yet.
void ui_obj_show(lv_obj_t *o, bool show);

// ---------------------------------------------------------------------------
// UiBanner — a transient, auto-dismissing message strip inside a tab.
//
// Used by the Color tab and the Virtuals tab to surface action failures where
// the user is already looking, instead of only on the bottom status bar.
// show() restarts the dismiss timer, so back-to-back failures keep the banner
// up for the full timeout rather than inheriting the first one's deadline.
// ---------------------------------------------------------------------------
class UiBanner {
public:
    // Creates the (hidden) label under `parent`. Call once, from the owning
    // module's build().
    void build(lv_obj_t *parent, lv_coord_t width);

    void show(const char *msg);   // show + (re)arm the auto-hide timer
    void hide();                  // hide + disarm

    // Re-color from the current theme. Call from the module's apply_theme().
    void apply_theme();

    // The underlying label, for owners that need to position it themselves
    // (the Virtuals tab pins it above its bottom toolbar). Null before build().
    lv_obj_t *obj() const { return _obj; }

private:
    static void hide_cb(lv_timer_t *t);
    void disarm();

    lv_obj_t   *_obj   = nullptr;
    lv_timer_t *_timer = nullptr;
};

// ---------------------------------------------------------------------------
// UiColorPicker — colorwheel + R/G/B sliders + live-preview swatch, all
// driving one shared RGB value.
//
// The Color tab and the per-virtual color modal are the same control at
// different sizes with different commit behaviour, so the geometry is passed
// in and the owner supplies an on_change hook (the tab throttles a live apply
// there; the modal does nothing and waits for its Apply button).
//
// The widgets are appended to `parent` in creation order — wheel, three
// slider rows, swatch — so build() must be called at the point in the
// parent's flex column where those should appear.
// ---------------------------------------------------------------------------
class UiColorPicker {
public:
    struct Metrics {
        lv_coord_t wheel_px;    // colorwheel is square
        lv_coord_t slider_w;
        lv_coord_t row_h;
        lv_coord_t swatch_h;
    };

    // Fired after any user-driven change, once every widget is back in sync.
    using ChangeCb = void (*)(void *user);

    void build(lv_obj_t *parent, const Metrics &m, ChangeCb on_change, void *user);

    // Forget the built widgets without touching them, keeping the current RGB
    // value. Owners whose parent container is deleted (the per-virtual modal
    // tears itself down on close) MUST call this, otherwise the next set_rgb()
    // writes through pointers to freed objects.
    void reset();

    // Set the value programmatically (seeding, or the "Black" shortcut) and
    // push it to every widget. Does NOT fire on_change — the caller already
    // knows, and re-entering the commit path from here would double-send.
    void set_rgb(LedColor c);

    LedColor rgb() const { return _c; }

    // Re-color the theme-dependent chrome (currently the swatch border).
    void apply_theme();

private:
    static UiColorPicker *from(lv_event_t *e);
    static void wheel_cb(lv_event_t *e);
    static void slider_r_cb(lv_event_t *e);
    static void slider_g_cb(lv_event_t *e);
    static void slider_b_cb(lv_event_t *e);

    void on_slider(int channel, int value);
    void refresh_preview();       // swatch + the three "R nnn" labels
    void sync_sliders();

    LedColor  _c = {255, 200, 128};
    lv_obj_t *_wheel     = nullptr;
    lv_obj_t *_slider[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *_label[3]  = {nullptr, nullptr, nullptr};
    lv_obj_t *_swatch    = nullptr;
    ChangeCb  _on_change = nullptr;
    void     *_user      = nullptr;
};
