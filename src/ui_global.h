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

// Reflect server-side global state (pause/brightness/mirror/flip) onto the
// Global-tab controls. Called from the result pump on RES_VIRTUALS.
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

// Getter for the top-right WiFi indicator (the only widget the Global tab
// places on lv_layer_top outside its own tab body). Exposed as a getter
// so the underlying `static` symbol stays file-local; the theme module
// reads this during ui_theme_apply() to re-color the indicator.
lv_obj_t *ui_global_conn_dot(void);
