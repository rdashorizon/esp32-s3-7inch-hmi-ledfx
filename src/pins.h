// ---------------------------------------------------------------------------
// pins.h — Elecrow CrowPanel ESP32 HMI 7.0-inch pin map
// Single source of truth for every GPIO used on the board.
//
// Sourced from the Elecrow wiki page for the 7.0-inch HMI display,
// https://www.elecrow.com/wiki/esp32-display-702727-intelligent-touch-screen-wi-fi26ble-800480-hmi-display.html
// (also DIS08070H). Tested on revisions V2.0 and V3.0 — the GT911 I2C
// driver is identical between the two; only the GT911 reset sequence
// differs (V3.0 uses a PCA9557 touch timing controller). See touch.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

// LCD backlight (PWM-driven for brightness slider if added later).
#define LCD_BL_PIN         2

// RGB parallel data lines (16-bit RGB565 in two transfers).
// Order is B0..B4, G0..G5, R0..R4 from the wiki LGFX reference.
#define LCD_PIN_B0  15
#define LCD_PIN_B1   7
#define LCD_PIN_B2   6
#define LCD_PIN_B3   5
#define LCD_PIN_B4   4

#define LCD_PIN_G0   9
#define LCD_PIN_G1  46
#define LCD_PIN_G2   3
#define LCD_PIN_G3   8
#define LCD_PIN_G4  16
#define LCD_PIN_G5   1

#define LCD_PIN_R0  14
#define LCD_PIN_R1  21
#define LCD_PIN_R2  47
#define LCD_PIN_R3  48
#define LCD_PIN_R4  45

// Sync lines.
#define LCD_PIN_DE   41   // data enable
#define LCD_PIN_VSYNC 40
#define LCD_PIN_HSYNC 39
#define LCD_PIN_PCLK   0

// RGB timing (from the wiki reference).
#define LCD_PCLK_HZ        15000000UL
#define LCD_HSYNC_FRONT_PORCH 40
#define LCD_HSYNC_PULSE_WIDTH 48
#define LCD_HSYNC_BACK_PORCH  40
#define LCD_VSYNC_FRONT_PORCH  1
#define LCD_VSYNC_PULSE_WIDTH 31
#define LCD_VSYNC_BACK_PORCH  13
#define LCD_HSYNC_POLARITY    0
#define LCD_VSYNC_POLARITY    0
#define LCD_PCLK_ACTIVE_NEG   1
#define LCD_DE_IDLE_HIGH      0
#define LCD_PCLK_IDLE_HIGH    0

// GT911 capacitive touch (I2C).
#define TOUCH_I2C_SDA   19
#define TOUCH_I2C_SCL   20
#define TOUCH_I2C_HZ    400000UL
#define TOUCH_ADDR      0x14   // GT911 default 7-bit address on this board
#define TOUCH_INT_PIN   -1     // wired on V3.0 via PCA9557; polled otherwise
#define TOUCH_RST_PIN   -1     // V2.0: not connected; V3.0: via PCA9557
