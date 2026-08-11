// ---------------------------------------------------------------------------
// ui_global.h — Global screen + persistent top-layer widgets + dim/splash
//
// The Global tab is one concern; the panel-backlight dim timer, the
// splash overlay, and the connection indicator are top-layer widgets that
// the Global screen coordinates (they share state with the screen-brightness
// slider on the Global tab). Co-locating them keeps that coupling local.
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include "ledfx.h"   // GlobalsState

// Builds the Global tab under the given parent (the third lv_tabview tab).
void ui_global_build(lv_obj_t *parent);

// Reflect server-side global state (brightness/mirror/flip) onto the
// Global-tab controls. Called from the result pump on RES_VIRTUALS. No-op if
// `g` isn't valid; mirror/flip are only written when an active effect actually
// reported them, so a server with nothing running doesn't force them off.
void ui_global_apply_state(const GlobalsState &g);

// Same as ui_global_apply_state, but also drives the Virtuals-tab pause switch
// (the switch lives on a different tab but its checked-state mirrors the
// server's `paused` flag). Used by the result pump.
void ui_global_apply_state_with_pause(const GlobalsState &g, lv_obj_t *pause_sw);

// Update the link-state indicator + the connection-status line. Called from
// the result pump on RES_CONN.
void ui_global_set_link(bool connected);

// Read-only view of the link state. Mirrored from RES_CONN messages; the UI
// modules use this for auto-refresh gating ("only refresh the front tab if
// connected") without reaching across into ui_global's state.
bool ui_global_link_ok(void);

// Refresh the "Last refresh Xs ago" line. Called when a data fetch completes
// or when the status timer ticks.
void ui_global_mark_refreshed(void);

// Periodic tick: updates the "Xs ago" line and re-paints the connection
// indicator. Wire to an LVGL timer from ui_init().
void ui_global_status_tick(void);

// One-shot dim/install: creates the auto-dim timer and the boot splash.
// Called from ui_init() after ui_global_build().
void ui_global_install_overlays(void);

// Theme support (Tier 1.4): re-color the Global tab's status row, the theme
// picker's panel/border, the "currently selected" button highlight, and the
// top-layer WiFi indicator (whose color depends on the theme mode too).
// Dispatched by ui_theme_apply() via the ui_global namespace hook.
void ui_global_apply_theme(void);

namespace ui_global { void apply_theme(); }
