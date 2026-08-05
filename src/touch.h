// ---------------------------------------------------------------------------
// touch.h — GT911 capacitive touch driver
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <lvgl.h>

void touch_init(void);
void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
