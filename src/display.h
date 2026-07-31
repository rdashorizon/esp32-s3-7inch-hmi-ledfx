// ---------------------------------------------------------------------------
// display.h — LovyanGFX panel + backlight
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB _bus_instance;
    lgfx::Panel_RGB _panel_instance;

    LGFX(void);
};

extern LGFX lcd;

void display_init(void);
void display_set_backlight(uint8_t level);  // 0..255
