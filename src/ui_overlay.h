// ---------------------------------------------------------------------------
// ui_overlay.h — top-layer overlay widgets (slow-network warning)
//
// Currently exposes one widget: a translucent "still working…" overlay that
// appears when a worker submit hasn't been followed by a Result within
// SLOW_THRESHOLD_MS. It only knows about submit/result cadence — it does NOT
// know what request is in flight or which tab the user is on.
//
// Owned by the LVGL thread (core 1). The worker thread must not touch this.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

// Called by ui.cpp when worker_submit() returns true (a request was queued).
void ui_overlay_mark_submit(void);

// Called by the result pump on every Result it drains (success or failure —
// any result counts as progress).
void ui_overlay_mark_progress(void);

// Install the periodic check that escalates to the slow-warning overlay.
// Called from ui_init().
void ui_overlay_install(void);
