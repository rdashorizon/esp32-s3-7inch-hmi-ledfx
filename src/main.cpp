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
#include "worker.h"

Config g_config;

// g_ledfx is defined once in ledfx.cpp and declared extern in ledfx.h.

void setup(void) {
    Serial.begin(115200);
    delay(200);

    display_init();
    touch_init();
    lvgl_init();

    if (!config_store.load(g_config)) {
        // No config yet — straight into the setup AP. The portal now services
        // LVGL so the screen stays live while the user fills in the form.
        ui_init();
        ui_show_status("Configure via WiFi: ledfx-hmi-setup");
        (void)config_store.run_captive_portal();
    }

    // We have config. Bring up the network worker first (so the very first UI
    // fetch has a queue to land in), then the UI. All connecting/login now
    // happens on the worker's core so the render loop never blocks on it.
    g_ledfx.set_base_url(g_config.ledfx_url);
    worker_init();
    worker_submit(req_simple(REQ_CONNECT));

    ui_init();  // enqueues the initial scenes fetch + starts the result pump
}

void loop(void) {
    lvgl_tick();
    delay(5);
}
