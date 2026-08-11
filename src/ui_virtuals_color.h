// ---------------------------------------------------------------------------
// ui_virtuals_color.h — per-virtual color picker modal
//
// Extracted from ui_virtuals.cpp when the module crossed ~570 LOC. The
// modal is a top-layer overlay anchored to the Virtuals tab; tapping a row's
// gradient swatch opens it, Apply commits via REQ_SET_VIRTUAL_COLOR, Cancel
// closes without sending. The modal owns the last-set row index (for the
// green confirmation flash in the parent module) and the persisted seed
// color in NVS.
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>
#include "ledfx.h"   // VirtualInfo (for ui_virtuals_color_open parameter)

// Hand-picked "signature" RGB565 hex for each LedFx gradient name. Used to
// color the row swatch and to seed the per-virtual color modal when the user
// hasn't picked anything yet. Lives here (rather than in ui_virtuals.h) so
// the color module owns everything color-related.
uint32_t ui_virtuals_color_gradient_to_rgb565(const String &name);

// Open the per-virtual color picker modal for the given virtual. Single-
// instance guarded — a second call closes the first modal before opening
// the new one. No-op if idx is out of range.
//
// The caller passes the VirtualInfo (rather than the module reading it
// from `ui_virtuals`'s internal array) to keep the modules decoupled —
// `ui_virtuals` owns the virtual list, `ui_virtuals_color` owns the modal.
// `v` is only borrowed for the duration of the call: the modal copies the id
// and effect type it needs, because the caller's array is freed and replaced
// on the next fetch, which can land while the modal is still open.
void ui_virtuals_color_open(int idx, const VirtualInfo &v);

// True if the row at `idx` was just color-applied (within FLASH_TIMEOUT_MS
// of millis()). The parent Virtuals module calls this on each render to flash
// the updated row's border in the accent color. Encapsulates the flash-window
// check so the parent doesn't need to know the timestamp.
bool ui_virtuals_color_row_is_recent(int idx);
