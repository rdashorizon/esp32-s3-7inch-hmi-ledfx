// ---------------------------------------------------------------------------
// ui_settings.h — on-device settings editor (separate LVGL screen)
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#pragma once

#include <lvgl.h>

// Opens the on-device settings editor as a separate LVGL screen.
// Save persists to NVS and reboots; Back returns to the tabbed UI.
void ui_settings_open(void);

// Wires the given button so a click opens the settings editor. Called from
// ui_init() for the "Settings" button on the Global screen.
void ui_settings_register_button(lv_obj_t *btn);
