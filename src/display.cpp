// ---------------------------------------------------------------------------
// display.cpp — LovyanGFX RGB panel for the CrowPanel 7.0-inch HMI
// Pin map: see pins.h. Timing: 800x480, 15 MHz pixel clock, EK9716BD3 +
// EK73002ACGB. The LGFX init block is the canonical example from the
// Elecrow wiki, with the constants factored into pins.h so the file
// compiles identically on V2.0 and V3.0 boards.
// ---------------------------------------------------------------------------
#include "display.h"
#include "pins.h"

LGFX lcd;

LGFX::LGFX(void) {
    {
        auto cfg = _bus_instance.config();
        cfg.panel = &_panel_instance;

        cfg.pin_d0  = GPIO_NUM_15; // B0
        cfg.pin_d1  = GPIO_NUM_7;  // B1
        cfg.pin_d2  = GPIO_NUM_6;  // B2
        cfg.pin_d3  = GPIO_NUM_5;  // B3
        cfg.pin_d4  = GPIO_NUM_4;  // B4

        cfg.pin_d5  = GPIO_NUM_9;  // G0
        cfg.pin_d6  = GPIO_NUM_46; // G1
        cfg.pin_d7  = GPIO_NUM_3;  // G2
        cfg.pin_d8  = GPIO_NUM_8;  // G3
        cfg.pin_d9  = GPIO_NUM_16; // G4
        cfg.pin_d10 = GPIO_NUM_1;  // G5

        cfg.pin_d11 = GPIO_NUM_14; // R0
        cfg.pin_d12 = GPIO_NUM_21; // R1
        cfg.pin_d13 = GPIO_NUM_47; // R2
        cfg.pin_d14 = GPIO_NUM_48; // R3
        cfg.pin_d15 = GPIO_NUM_45; // R4

        cfg.pin_henable = GPIO_NUM_41;
        cfg.pin_vsync   = GPIO_NUM_40;
        cfg.pin_hsync   = GPIO_NUM_39;
        cfg.pin_pclk    = GPIO_NUM_0;

        cfg.freq_write  = LCD_PCLK_HZ;

        cfg.hsync_polarity    = LCD_HSYNC_POLARITY;
        cfg.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
        cfg.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
        cfg.hsync_back_porch  = LCD_HSYNC_BACK_PORCH;

        cfg.vsync_polarity    = LCD_VSYNC_POLARITY;
        cfg.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
        cfg.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
        cfg.vsync_back_porch  = LCD_VSYNC_BACK_PORCH;

        cfg.pclk_active_neg   = LCD_PCLK_ACTIVE_NEG;
        cfg.de_idle_high      = LCD_DE_IDLE_HIGH;
        cfg.pclk_idle_high    = LCD_PCLK_IDLE_HIGH;

        _bus_instance.config(cfg);
    }

    {
        auto cfg = _panel_instance.config();
        cfg.memory_width  = 800;
        cfg.memory_height = 480;
        cfg.panel_width   = 800;
        cfg.panel_height  = 480;
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        _panel_instance.config(cfg);
    }

    _panel_instance.setBus(&_bus_instance);

    {
        // Backlight (GPIO2), PWM-driven so display_set_backlight() can dim it.
        auto cfg = _light_instance.config();
        cfg.pin_bl = GPIO_NUM_2;
        _light_instance.config(cfg);
    }
    _panel_instance.light(&_light_instance);

    setPanel(&_panel_instance);
}

void display_init(void) {
    lcd.init();
    lcd.setColorDepth(16);  // RGB565
    lcd.fillScreen(TFT_BLACK);
    lcd.setBrightness(255);
}

void display_set_backlight(uint8_t level) {
    lcd.setBrightness(level);
}
