// ---------------------------------------------------------------------------
// touch.h — GT911 capacitive touch driver
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <lvgl.h>

void touch_init(void);
bool touch_read(lv_indev_t *indev, lv_indev_data_t *data);
