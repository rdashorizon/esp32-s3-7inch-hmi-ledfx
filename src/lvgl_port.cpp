// ---------------------------------------------------------------------------
// lvgl_port.cpp — LVGL ↔ LovyanGFX + GT911 wiring
//
// Notes:
// - LVGL v8.3 is used because the LovyanGFX RGB bus produces RGB565, and
//   lv_conf.h falls back to RGB565 swizzle anyway. PSRAM hosts the draw
//   buffer so this fits comfortably on the 8 MB S3-N4R8.
// - We use two 1/10-line partial buffers (15 KB each for 800×48 pixels)
//   so the LVGL task can run on the same thread as the loop without
//   starving the touch read.
// ---------------------------------------------------------------------------
#include "lvgl_port.h"
#include "display.h"
#include "touch.h"
#include <lvgl.h>

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.pushColors((uint16_t *)color_p, w * h, true);
    lcd.endWrite();
    lv_disp_flush_ready(drv);
}

static uint32_t my_tick(void) {
    return millis();
}

void lvgl_init(void) {
    lv_init();

    // Two 1/10-line buffers in PSRAM.
    const uint32_t lines = 48;
    const uint32_t pixels = 800 * lines;
    buf1 = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 800;
    disp_drv.ver_res = 480;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);
}

void lvgl_tick(void) {
    uint32_t ms = my_tick();
    lv_tick_inc(ms);
    lv_timer_handler();
}
