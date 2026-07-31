// ---------------------------------------------------------------------------
// ui.h — LVGL screen builders
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <lvgl.h>

void ui_init(void);

void ui_show_status(const char *msg, bool is_error = false);
void ui_refresh_scenes(void);   // re-fetch + repaint scene grid
void ui_refresh_virtuals(void); // re-fetch + repaint virtual list
