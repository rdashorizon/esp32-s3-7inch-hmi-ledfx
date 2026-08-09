// ---------------------------------------------------------------------------
// ui.h — LVGL screen builders
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <lvgl.h>

void ui_init(void);

void ui_show_status(const char *msg, bool is_error = false);
void ui_show_status_fmt(bool is_error, const char *fmt, ...);  // printf-style
void ui_refresh_scenes(void);   // re-fetch + repaint scene grid
void ui_refresh_virtuals(void); // re-fetch + repaint virtual list

// The root screen object, for any child module that needs to load it
// (e.g. settings editor's "Back" returns here). Exposed for ui_settings.cpp;
// not part of the public UI surface — don't touch from outside ui.cpp /
// per-screen modules.
lv_obj_t *ui_root(void);
