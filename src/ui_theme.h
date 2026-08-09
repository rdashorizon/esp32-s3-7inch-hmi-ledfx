// ---------------------------------------------------------------------------
// ui_theme.h — Dark/Light mode + accent color presets
//
// Owns the color maps and the apply logic. Loaded once at boot (via
// ui_theme_setup, called from main.cpp BEFORE ui_init so the first paint
// uses the right palette). At runtime, the Global-tab Theme picker calls
// ui_theme_set_and_apply() on every tap; per-screen modules expose
// apply_theme() hooks in their own namespaces that this function dispatches to.
//
// Owned by the LVGL thread (core 1). Does not call into the network client;
// just repaints widgets.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include "config.h"   // Theme, ThemeMode, AccentColor

// Load the theme from NVS into module-static state. Call once in main.cpp
// after display_init + lvgl_init but BEFORE ui_init so the first paint
// uses the right palette (no flash-of-wrong-theme).
void ui_theme_setup();

// Update + persist + apply. Called from the Global-tab Theme picker on
// every tap. Internally debounced (~200 ms) so rapid taps don't cause
// paint thrash — a real-world user clicking through accents won't hit
// the throttle.
void ui_theme_set_and_apply(const Theme &t);

// Apply the currently-loaded theme to every visible widget. Per-screen
// modules expose apply_theme() hooks that this function dispatches to.
// Safe to invoke at any time on the LVGL thread.
void ui_theme_apply();

// Read-only view of the loaded theme.
const Theme &ui_theme_current();

// RGB565 hex for the current accent color. Used by per-screen apply_theme
// hooks that need to paint "the active color" without knowing which
// preset the user picked. Single source of truth for accent hex.
uint32_t ui_theme_accent_rgb565();

// Friendly name for the current accent (e.g. "Blue"). Used by the
// picker's status label / confirmations.
const char *ui_theme_accent_name();

// Palette accessors used by per-screen apply_theme() hooks. All return
// RGB565 hex for the currently-active theme mode.
uint32_t ui_theme_root_bg();
uint32_t ui_theme_panel_bg();
uint32_t ui_theme_panel_alt();
uint32_t ui_theme_text_primary();
uint32_t ui_theme_text_muted();
uint32_t ui_theme_text_error();
uint32_t ui_theme_text_warn();
uint32_t ui_theme_border();
