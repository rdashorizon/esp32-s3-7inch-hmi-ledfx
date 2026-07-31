// ---------------------------------------------------------------------------
// lvgl_port.h — LVGL 8.x binding to LovyanGFX + GT911
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

void lvgl_init(void);
void lvgl_tick(void);  // call from loop()
