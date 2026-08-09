// ---------------------------------------------------------------------------
// ui_color.h — Color picker tab (LVGL colorwheel + RGB sliders)
//
// Drives the global LedFx LED color via PUT /api/effects {"action":
// "apply_global","background_color":"#rrggbb"} — confirmed against
// LedFx/docs/apis/global.md. Throttled live preview during drag; one
// HTTP request on release.
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>

// Build the Color tab under the given parent. Adds the colorwheel, the RGB
// sliders, the live-preview swatch, the Apply + Black buttons, and the
// transient error banner.
void ui_color_build(lv_obj_t *parent);

// Enqueue an apply_global with the current color. Called by:
//   - the Apply button (manual send)
//   - the release handler on the colorwheel / sliders (after a drag settles)
//   - the throttled live-apply path during a drag
//   - ui_init() if a saved color exists (see Tier 1.6)
void ui_color_apply_now(void);

// Adopt the worker's result of the most recent apply (success/failure). On
// non-200, shows the transient error banner on the Color tab.
void ui_color_pump_result(int status, const char *msg);

// Theme support (Tier 1.3): re-color the persistent widgets (swatch border,
// banner, Apply / Black buttons) to the current theme.
void ui_color_apply_theme(void);

namespace ui_color { void apply_theme(); }
