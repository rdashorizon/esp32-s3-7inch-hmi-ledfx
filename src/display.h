// ---------------------------------------------------------------------------
// display.h — LovyanGFX panel + backlight
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
// The ESP32-S3 RGB parallel bus + panel classes are not pulled in by
// <LovyanGFX.hpp>; they live in platform-specific headers that must be
// included explicitly (this is what the bundled lgfx_user board profiles do).
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB _bus_instance;
    lgfx::Panel_RGB _panel_instance;
    // Backlight on GPIO2. Without a registered light, setBrightness() is a
    // no-op and the panel stays dark even though pixels are being driven.
    lgfx::Light_PWM _light_instance;

    LGFX(void);
};

extern LGFX lcd;

void display_init(void);
void display_set_backlight(uint8_t level);  // 0..255
