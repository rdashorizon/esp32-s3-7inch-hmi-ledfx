// ---------------------------------------------------------------------------
// main.cpp — setup + loop; the entire app is the coordination between
// the display, the touch, the LVGL tick, the WiFi/network layer, the LedFx
// client, and the UI.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <ArduinoJson.h>
#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "config.h"
#include "net.h"
#include "ledfx.h"
#include "ui.h"
#include "ui_theme.h"   // ui_theme_setup() — Tier 1.5: load saved theme before ui_init
#include "worker.h"
#include "ota.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

Config g_config;

// g_ledfx is defined once in ledfx.cpp and declared extern in ledfx.h.

// Periodic heap + PSRAM free-space printout, pinned to core 0 so it never
// disturbs the LVGL render loop on core 1. Useful for catching fragmentation
// regressions in the field.
static void heapmon_task(void *) {
    for (;;) {
#if defined(ARDUINO_ARCH_ESP32)
        Serial.printf("[heap] free=%u spiram_free=%u\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void setup(void) {
    Serial.begin(115200);
    delay(200);

    display_init();
    touch_init();
    lvgl_init();

    // Load the saved theme from NVS into module-static state BEFORE ui_init
    // so the very first paint uses the right palette (no flash of wrong
    // theme). The Theme is module-static; per-screen apply_theme() hooks
    // read from it when they build their widgets.
    ui_theme_setup();

    if (!config_store.load(g_config)) {
        // No config yet — straight into the setup AP. The portal services LVGL
        // so the screen stays live while the user fills in the form, and it
        // reboots the device as soon as a config is saved.
        ui_init();
        ui_show_status("Configure via WiFi: ledfx-hmi-setup");
        (void)config_store.run_captive_portal();
        // run_captive_portal() only returns if it was given a timeout (it
        // isn't here). Return rather than falling through — the rest of setup()
        // would call ui_init() a second time and build a duplicate tabview,
        // duplicate timers and duplicate overlays on top of the first.
        return;
    }

    // We have config. Bring up the network worker first (so the very first UI
    // fetch has a queue to land in), then the UI. All connecting/login now
    // happens on the worker's core so the render loop never blocks on it.
    g_ledfx.set_base_url(g_config.ledfx_url);
    worker_init();
    worker_submit(req_simple(REQ_CONNECT));

    ui_init();  // enqueues the initial scenes fetch + starts the result pump

    // Re-apply the last-picked color to LedFx so the LED strip matches the
    // device's display state on boot. Skipped if the saved color is the
    // warm-white default (first boot) — that would just be noise on a fresh
    // install where the user hasn't picked anything yet.
    {
        LedColor saved = config_store.load_last_color();
        bool is_default = (saved.r == 255 && saved.g == 200 && saved.b == 128);
        if (!is_default) {
            StaticJsonDocument<128> doc;
            doc["action"] = "apply_global";
            char hex[8];
            snprintf(hex, sizeof(hex), "#%02x%02x%02x", saved.r, saved.g, saved.b);
            doc["background_color"] = hex;
            String body;
            serializeJson(doc, body);
            worker_submit(req_payload(REQ_APPLY_GLOBAL, body));
            Serial.printf("[boot] re-applying saved color #%02x%02x%02x\n",
                          saved.r, saved.g, saved.b);
        }
    }

    // Configure network OTA (Tier 4). The listener starts lazily once WiFi is
    // up (ota_loop), so this just registers the hostname/password/callbacks;
    // safe to call before the link is established.
    ota_setup();

#if defined(ARDUINO_ARCH_ESP32)
    // Heap monitor on core 0 — 2 KB stack is enough for the printf-only task.
    xTaskCreatePinnedToCore(heapmon_task, "heapmon", 2048, nullptr, 1, nullptr, 0);
#endif
}

void loop(void) {
    lvgl_tick();
    ota_loop();   // service network OTA (cheap when idle; blocks during a flash)
    delay(5);
}
