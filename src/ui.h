// ---------------------------------------------------------------------------
// ui.h — LVGL screen builders
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <lvgl.h>

struct Request;  // defined in worker.h (forward decl keeps ui.h light)

// Tab order in the tabview built by ui_init(). Also the value persisted by
// ConfigStore::save_last_tab(). Keep in sync with the lv_tabview_add_tab()
// calls in ui_init().
enum UiTab : uint8_t {
    UI_TAB_SCENES = 0,
    UI_TAB_VIRTUALS,
    UI_TAB_COLOR,
    UI_TAB_GLOBAL,
    UI_TAB_COUNT,
};

void ui_init(void);

void ui_show_status(const char *msg, bool is_error = false);
void ui_show_status_fmt(bool is_error, const char *fmt, ...);  // printf-style

// Getter for the bottom status label. Exposed as a getter (not an extern
// global) so the underlying symbol can stay `static` in ui.cpp and we don't
// widen the global namespace. The theme module reads this during
// ui_theme_apply() to re-color the label.
lv_obj_t *ui_status_label(void);

// UI-side wrapper for worker_submit() that also notifies the slow-network
// overlay. Prefer this over calling worker_submit() directly from UI code.
bool ui_submit(const Request &req);

// The root screen object, for any child module that needs to load it
// (e.g. settings editor's "Back" returns here). Exposed for ui_settings.cpp;
// not part of the public UI surface — don't touch from outside ui.cpp /
// per-screen modules.
lv_obj_t *ui_root(void);
