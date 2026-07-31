// ---------------------------------------------------------------------------
// main.cpp — setup + loop; the entire app is the coordination between
// the display, the touch, the LVGL tick, the WiFi/network layer, the LedFx
// client, and the UI.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "config.h"
#include "net.h"
#include "ledfx.h"
#include "ui.h"

Config g_config;

LedFxClient g_ledfx;

static void attempt_login_or_revert(uint8_t retries) {
    for (uint8_t i = 0; i < retries; i++) {
        if (!net.connect_wifi(g_config.wifi_ssid, g_config.wifi_pass)) {
            ui_show_status("WiFi failed, retrying...", true);
            delay(2000);
            continue;
        }
        if (g_ledfx.connect(g_config.ledfx_user, g_config.ledfx_pass)) {
            ui_show_status("Connected to LedFx");
            return;
        }
        ui_show_status("LedFx login failed, retrying...", true);
        delay(2000);
    }
    // Fall back to setup mode.
    config_store.clear();
    (void)config_store.run_captive_portal();
}

void setup(void) {
    Serial.begin(115200);
    delay(200);

    display_init();
    touch_init();
    lvgl_init();

    if (!config_store.load(g_config)) {
        // No config yet — straight into the setup AP.
        ui_init();
        ui_show_status("Configure via WiFi: ledfx-hmi-setup");
        (void)config_store.run_captive_portal();
    }

    // We have config. Init the UI, then try to connect.
    ui_init();

    g_ledfx.set_base_url(g_config.ledfx_url);
    attempt_login_or_revert(3);
}

void loop(void) {
    lvgl_tick();
    delay(5);
}
