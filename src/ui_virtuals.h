// ---------------------------------------------------------------------------
// ui_virtuals.h — Virtuals tab (list of every LedFx virtual)
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include "ledfx.h"   // VirtualInfo, GlobalsState

// Build the Virtuals tab under the given parent. Also creates the toolbar
// (Pause-all switch + Clear-all button) and the spinner/error banner.
void ui_virtuals_build(lv_obj_t *parent);

// Enqueue REQ_FETCH_VIRTUALS via the worker. Called by:
//   - ui_init() (kick off the initial fetch — virtuals carry the global state
//     that seeds the Global-tab controls)
//   - tab_changed_cb() when switching to this tab
//   - auto_refresh_cb() (periodic refresh of the front tab)
//   - the worker, after every virtual-scene action (to reflect new state)
void ui_virtuals_request_refresh(void);

// Adopt a freshly fetched virtual list from the worker Result. Ownership of
// the array passes to this module (delete[] on the next call). On status==200
// we also call ui_global_apply_state_with_pause() so the Global-tab pause
// switch stays in sync with the server.
void ui_virtuals_pump_result(int status, int count, VirtualInfo *data,
                             const GlobalsState &globals, const char *err);

// Adopt the result of any RES_ACTION while the Virtuals tab is in front
// (Tier 1.6). On non-200, shows a transient error banner on the tab so the
// user notices a color-set or toggle failure without checking the bottom
// status label.
void ui_virtuals_pump_action_result(int status, const char *msg);

// Theme support: re-color the persistent widgets (spinner, error banner,
// in-tab action banner) and re-render the virtual list. Row swatch borders
// and the confirmation flash are baked in at build time, so the list has to
// be rebuilt rather than merely invalidated.
void ui_virtuals_apply_theme(void);

namespace ui_virtuals { void apply_theme(); }
