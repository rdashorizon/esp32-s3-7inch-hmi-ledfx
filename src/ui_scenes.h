// ---------------------------------------------------------------------------
// ui_scenes.h — Scenes tab (4-column grid of saved LedFx scenes)
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include "ledfx.h"   // SceneInfo

// Build the Scenes tab under the given parent (the first lv_tabview tab).
// Also creates the spinner + error banner overlaid on the tab.
void ui_scenes_build(lv_obj_t *parent);

// Enqueue REQ_FETCH_SCENES via the worker. Called by:
//   - ui_init()  (kick off the initial fetch)
//   - tab_changed_cb() when switching to this tab
//   - auto_refresh_cb() (periodic refresh of the front tab)
//   - the result pump's RES_VIRTUALS handler (we re-fetch scenes after every
//     scene action, mirroring LedFx state)
void ui_scenes_request_refresh(void);

// Adopt a freshly fetched scene list from the worker Result. The pump calls
// this on RES_SCENES; ownership of the array passes to this module (delete[]
// on the next call). status == 200 means success; non-200 paints the error
// banner instead.
void ui_scenes_pump_result(int status, int count, SceneInfo *data, const char *err);

// Theme support: re-color the persistent widgets (spinner + error banner) and
// re-render the scene grid. The active-scene highlight is baked into each
// button at build time, so the grid has to be rebuilt rather than merely
// invalidated.
void ui_scenes_apply_theme(void);

// Group of public hooks exposed in namespace ui_scenes for cross-module
// dispatch from ui_theme_apply().
namespace ui_scenes { void apply_theme(); }
