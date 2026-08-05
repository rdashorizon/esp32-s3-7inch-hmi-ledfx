// ---------------------------------------------------------------------------
// lv_conf.h — LVGL v8.x configuration for the CrowPanel 7" HMI.
//
// LVGL requires this file to exist on the include path whenever
// LV_CONF_INCLUDE_SIMPLE is defined (see build_flags in platformio.ini).
// Anything not overridden here falls back to the compiled-in defaults in
// LVGL's lv_conf_internal.h, so this file only pins the handful of options
// that matter for this board:
//   - 16-bit RGB565 colour to match the LovyanGFX RGB bus
//   - a comfortably sized object memory pool
//   - the Montserrat 14 default font and the built-in symbol glyphs
//     (LV_SYMBOL_SHUFFLE is used on the Virtuals screen)
// ---------------------------------------------------------------------------
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Colour depth. LV_COLOR_DEPTH may already be supplied via build_flags; only
// define it here if it was not, to avoid a redefinition diagnostic.
#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif

// LovyanGFX pushes native RGB565, so no byte swap is required in LVGL.
#define LV_COLOR_16_SWAP 0

// Object memory pool (widgets, styles, animations). The frame buffers live in
// PSRAM and are allocated separately in lvgl_port.cpp, so this pool only has
// to hold the widget tree — 48 KB is plenty for these screens.
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

// Tick + refresh timing.
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

// Fonts. Montserrat 14 is the default UI font; the symbol glyphs it carries
// include LV_SYMBOL_SHUFFLE used by the Virtuals randomize button.
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Logging off in release builds; flip to LV_LOG_LEVEL_WARN while debugging.
#define LV_USE_LOG 0

#endif // LV_CONF_H
