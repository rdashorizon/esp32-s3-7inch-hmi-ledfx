// ---------------------------------------------------------------------------
// ui_color.cpp — Color picker tab implementation
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
//
// This file is filled in by Tier 1 tasks 1.3–1.5. The scaffold here just
// declares the API; ui_color_build() is empty until Task 1.3.
// ---------------------------------------------------------------------------
#include "ui_color.h"

void ui_color_build(lv_obj_t *parent) { (void)parent; }
void ui_color_apply_now(void) {}
void ui_color_pump_result(int status, const char *msg) {
    (void)status; (void)msg;
}
