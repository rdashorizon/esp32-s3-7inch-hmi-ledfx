// ---------------------------------------------------------------------------
// ui_theme.cpp — Dark/Light mode + accent color presets
//
// Owned by the LVGL thread (core 1). Does not call into the network client;
// just repaints widgets.
// ---------------------------------------------------------------------------
#include "ui_theme.h"
#include "ui.h"             // ui_status_label() getter
#include "ui_global.h"      // ui_global::apply_theme()
#include "ui_scenes.h"      // ui_scenes::apply_theme()
#include "ui_virtuals.h"    // ui_virtuals::apply_theme()
#include "ui_color.h"       // ui_color::apply_theme()
#include <Arduino.h>       // snprintf

// ---- Module-static state --------------------------------------------------
static Theme s_theme;

const Theme &ui_theme_current() { return s_theme; }

// ---- Accent color → RGB565 hex -------------------------------------------
// Single source of truth for "what does BLUE look like?" — adding a new
// accent preset touches only this map.
static uint32_t accent_to_rgb565(AccentColor a) {
    switch (a) {
        case AccentColor::BLUE:    return 0x2266cc;
        case AccentColor::GREEN:   return 0x22aa44;
        case AccentColor::ORANGE:  return 0xcc6622;
        case AccentColor::MAGENTA: return 0xcc22aa;
    }
    return 0x2266cc;
}

uint32_t ui_theme_accent_rgb565() { return accent_to_rgb565(s_theme.accent); }

const char *ui_theme_accent_name() {
    switch (s_theme.accent) {
        case AccentColor::BLUE:    return "Blue";
        case AccentColor::GREEN:   return "Green";
        case AccentColor::ORANGE:  return "Orange";
        case AccentColor::MAGENTA: return "Magenta";
    }
    return "?";
}

// ---- Palette table --------------------------------------------------------
// Every color used by the UI in the current theme. Per-screen apply_theme
// hooks read from these via the ui_theme_*() accessors; they don't compute
// their own colors. Adding a third mode (e.g. HIGH_CONTRAST) is a one-line
// addition here.
struct Palette {
    uint32_t root_bg;
    uint32_t panel_bg;
    uint32_t panel_alt;
    uint32_t text_primary;
    uint32_t text_muted;
    uint32_t text_error;
    uint32_t text_warn;
    uint32_t border;
};

static const Palette dark_palette = {
    0x0d0d14,  // root_bg       — tab root + settings editor background
    0x1a1a22,  // panel_bg      — inner panels (Color modal, settings fields)
    0x331111,  // panel_alt     — alt panel bg (in-tab error banner)
    0xffffff,  // text_primary  — default label color
    0xcccccc,  // text_muted    — secondary label / status bar
    0xff4444,  // text_error    — bottom-status error text
    0xff8888,  // text_warn     — in-tab warning banner text
    0x444444,  // border        — default 1-px border
};

static const Palette light_palette = {
    0xf0f0f4,  // root_bg
    0xffffff,  // panel_bg
    0xfff0f0,  // panel_alt
    0x1a1a1a,  // text_primary
    0x555555,  // text_muted
    0xcc2222,  // text_error
    0xaa3333,  // text_warn
    0xbbbbbb,  // border
};

static const Palette &current_palette() {
    return s_theme.mode == ThemeMode::LIGHT ? light_palette : dark_palette;
}

uint32_t ui_theme_root_bg()        { return current_palette().root_bg; }
uint32_t ui_theme_panel_bg()       { return current_palette().panel_bg; }
uint32_t ui_theme_panel_alt()      { return current_palette().panel_alt; }
uint32_t ui_theme_text_primary()   { return current_palette().text_primary; }
uint32_t ui_theme_text_muted()     { return current_palette().text_muted; }
uint32_t ui_theme_text_error()     { return current_palette().text_error; }
uint32_t ui_theme_text_warn()      { return current_palette().text_warn; }
uint32_t ui_theme_border()         { return current_palette().border; }

// ---- LVGL's own theme ------------------------------------------------------
// The palette above only covers widgets this app paints by hand — maybe a dozen
// of them. Everything else (the tabview, every lv_obj_create container, every
// button, switch, slider, textarea) is styled by LVGL's built-in theme, which
// lv_disp_drv_register() installs as:
//
//     lv_theme_default_init(disp, BLUE, RED, LV_THEME_DEFAULT_DARK, font)
//
// LV_THEME_DEFAULT_DARK is a *compile-time* flag and defaults to 0. So until
// this function existed, nearly every pixel on screen was locked to LVGL's
// light theme with a blue primary, and flipping our Dark/Light mode repainted
// only the handful of hand-styled widgets — which is why the Theme picker
// looked like it did nothing.
//
// Re-calling lv_theme_default_init() is the supported way to change it at
// runtime: it re-runs the theme's style_init() (leak-free — style_init_reset()
// resets already-inited styles) and, because the display is already using this
// theme, calls lv_obj_report_style_change(NULL) to restyle every live widget.
//
// Must run BEFORE the per-screen hooks below: those re-apply local styles, and
// local styles win over theme styles, so the hand-painted accents have to land
// on top of the fresh theme rather than under it.
static void apply_lvgl_theme() {
#if LV_USE_THEME_DEFAULT
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;
    lv_theme_default_init(disp,
                          lv_color_hex(ui_theme_accent_rgb565()),  // primary
                          lv_palette_main(LV_PALETTE_GREY),        // secondary
                          s_theme.mode == ThemeMode::DARK,
                          LV_FONT_DEFAULT);
#endif
}

// ---- Setup ----------------------------------------------------------------
void ui_theme_setup() {
    s_theme = config_load_theme();
    // Set LVGL's theme before ui_init() creates any widgets, so the first paint
    // is already correct instead of being restyled a frame later.
    apply_lvgl_theme();
}

// ---- Public API ------------------------------------------------------------
void ui_theme_set_and_apply(const Theme &t) {
    // No debounce here. The previous version assigned s_theme and *then*
    // returned early on a ~200 ms throttle, skipping both the NVS write and
    // the repaint — so a quick second tap left the in-memory theme disagreeing
    // with both the screen and NVS, and the change was lost on reboot.
    // Repainting a few dozen widgets is cheap; NVS is only written when the
    // value actually changes, so tapping through the presets costs nothing.
    bool changed = (t.mode != s_theme.mode) || (t.accent != s_theme.accent);
    s_theme = t;
    if (changed) config_save_theme(s_theme);
    ui_theme_apply();
    // Visual confirmation: paint the bottom status label with the new
    // theme summary so the user has an explicit "yes, that saved" signal
    // beyond the live repaint.
    char msg[48];
    snprintf(msg, sizeof(msg), "Theme: %s, %s",
        s_theme.mode == ThemeMode::DARK ? "Dark" : "Light",
        ui_theme_accent_name());
    ui_show_status(msg, false);
}

void ui_theme_apply() {
    const Palette &p = current_palette();

    // Restyle every stock widget first (see apply_lvgl_theme).
    apply_lvgl_theme();

    // The root screen's ground. Without this, switching to Light mode repainted
    // every widget but left the whole app sitting on the dark background.
    lv_obj_t *root = ui_root();
    if (root) lv_obj_set_style_bg_color(root, lv_color_hex(p.root_bg), 0);

    // Cross-screen persistent widgets.
    lv_obj_t *status = ui_status_label();
    if (status) {
        lv_obj_set_style_bg_color(status, lv_color_hex(p.root_bg), 0);
        lv_obj_set_style_text_color(status, lv_color_hex(p.text_muted), 0);
    }
    // Per-screen apply_theme() hooks (added in Tier 1.3 — the ui_scenes,
    // ui_virtuals, ui_color namespaces expose void apply_theme() functions).
    ui_scenes::apply_theme();
    ui_virtuals::apply_theme();
    ui_color::apply_theme();
    ui_global::apply_theme();
}
