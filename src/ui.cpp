// ---------------------------------------------------------------------------
// ui.cpp — LVGL screen builders
//
// Three screens, switched via a top tab bar:
//   1. Scenes — grid (4 cols) of saved scenes, tap to activate
//   2. Virtuals — list of every virtual, toggle + randomize
//   3. Global — brightness slider, mirror / flip toggles, gradient picker
//
// A bottom status bar shows WiFi state, LedFx server, last refresh time.
// ---------------------------------------------------------------------------
//
// THREADING OWNERSHIP
// All UI state lives inside one of the per-screen modules (ui_scenes,
// ui_virtuals, ui_global, ui_settings). Every state object (s_scenes,
// s_virt, s_pause_sw, s_link_ok, s_last_refresh_ms, s_bright_slider, etc.)
// is owned by the LVGL thread (core 1):
//   - result_pump_cb() is the only writer of fetched scene/virtual arrays
//     (it delegates to ui_scenes_pump_result / ui_virtuals_pump_result)
//   - ui_global_apply_state_with_pause() is the only writer of s_pause_sw's
//     checked-state (it routes server `paused` flag through ui_global)
//   - ui_global_set_link() / ui_global_mark_refreshed() are the only writers
//     of s_link_ok / s_last_refresh_ms
// The worker (core 0) MUST NOT touch any UI state directly; it goes through
// worker_submit() and posts a Result. If you need a new piece of cross-thread
// state, add it to the Result struct in worker.h and read it in the pump.
// ---------------------------------------------------------------------------
#include "ui.h"
#include "ledfx.h"     // SceneInfo / VirtualInfo
#include "worker.h"    // background network worker (submit/poll)
#include "config.h"
#include "display.h"   // display_set_backlight()
#include "ui_settings.h"  // on-device settings editor (extracted module)
#include "ui_global.h"    // Global tab + dim + splash + conn indicator
#include "ui_scenes.h"    // Scenes tab
#include "ui_virtuals.h"  // Virtuals tab + pause-all switch
#include <ArduinoJson.h>
#include <stdarg.h>       // va_list for ui_show_status_fmt()

extern Config g_config;  // see main.cpp

static lv_obj_t *s_root;
static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_scenes;
static lv_obj_t *s_tab_virtuals;
static lv_obj_t *s_tab_global;
static lv_obj_t *s_status_label;

// Refresh the active data screen (scenes/virtuals) on a timer so the HUD
// tracks LedFx state changes made elsewhere. Period in milliseconds.
static const uint32_t AUTO_REFRESH_MS = 30000;
// How often the UI drains network results from the worker and repaints.
static const uint32_t RESULT_PUMP_MS = 80;

// ---- Global screen ---------------------------------------------------------
// The Global tab is built and managed by ui_global.{h,cpp}. The Scenes and
// Virtuals tabs are built by ui_scenes.{h,cpp} and ui_virtuals.{h,cpp}
// respectively; this file is just the glue (tabview setup, result pump,
// status bar, splash).

// Show/hide an optional widget (spinner, error banner). Per-screen modules
// have their own local obj_show helpers — this one stays for any future
// shared use.
static void obj_show(lv_obj_t *o, bool show) {
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Enqueue a refresh for whichever data screen is currently in front.
static void request_active_screen(void) {
    uint32_t idx = lv_tabview_get_tab_act(s_tabview);
    if (idx == 0) ui_scenes_request_refresh();
    else if (idx == 1) ui_virtuals_request_refresh();
}

// Periodic refresh of the front data screen, plus a status tick.
static void auto_refresh_cb(lv_timer_t *t) {
    (void)t;
    ui_global_status_tick();
    if (ui_global_link_ok()) request_active_screen();
}

// Drain the worker's result queue on the LVGL thread and repaint. This is the
// only place scene/virtual data or link state is adopted into the UI.
static void result_pump_cb(lv_timer_t *t) {
    (void)t;
    Result r;
    while (worker_poll(r)) {
        switch (r.type) {
            case RES_SCENES:
                ui_scenes_pump_result(r.status, r.count,
                                      static_cast<SceneInfo *>(r.data), r.msg);
                break;
            case RES_VIRTUALS:
                ui_virtuals_pump_result(r.status, r.count,
                                         static_cast<VirtualInfo *>(r.data),
                                         r.globals, r.msg);
                break;
            case RES_ACTION:
                ui_show_status(r.msg, r.status != 200);
                break;
            case RES_CONN:
                ui_show_status(r.msg, !r.connected);
                ui_global_set_link(r.connected);
                break;
        }
    }
}

// Confirmation handler for the "Reset settings" button.
static void reset_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t id = lv_msgbox_get_active_btn(mbox);  // 0 = Reset, 1 = Cancel
    lv_msgbox_close(mbox);
    if (id == 0) {
        config_store.clear();  // wipe saved WiFi + LedFx config from NVS
        ui_show_status("Settings cleared — rebooting into setup…");
        lv_refr_now(NULL);     // paint the message before we restart
        delay(600);
        ESP.restart();         // reboot: no config -> captive portal
    }
}

// Wipe saved WiFi/LedFx settings and reboot into the setup portal. This is the
// supported way to re-enter setup on this board (the BOOT button can't be used
// — GPIO0 is the LCD pixel clock).
static void reset_btn_cb(lv_event_t *e) {
    (void)e;
    static const char *btns[] = {"Reset", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Reset settings",
        "Erase saved WiFi and LedFx settings and reboot into setup?", btns, false);
    lv_obj_add_event_cb(mbox, reset_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

// On-device panel backlight slider (separate from LedFx LED brightness) +
// auto-dim timer + boot splash overlay all live in ui_global.{h,cpp}.
// See ui_global_build(), ui_global_install_overlays().

// ---- Settings editor -------------------------------------------------------
// Edit WiFi/LedFx settings in place (no captive-portal round trip). The
// implementation lives in ui_settings.cpp — this file just registers the
// button on the Global screen. See ui_settings.h.

static void build_global_screen(void) {
    // The Global tab itself is built by ui_global_build(); the Settings +
    // Reset buttons stay here because Reset touches NVS, and Settings is
    // wired to the button by ui_settings_register_button().
    ui_global_build(s_tab_global);

    lv_obj_t *btnrow = lv_obj_create(s_tab_global);
    lv_obj_set_size(btnrow, LV_PCT(100), 66);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *settings_btn = lv_btn_create(btnrow);
    lv_obj_set_height(settings_btn, 50);
    lv_obj_t *stl = lv_label_create(settings_btn);
    lv_label_set_text(stl, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_center(stl);
    ui_settings_register_button(settings_btn);

    // Reset: wipe WiFi/LedFx config and reboot into the setup portal.
    lv_obj_t *reset_btn = lv_btn_create(btnrow);
    lv_obj_set_height(reset_btn, 50);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0xaa2222), 0);
    lv_obj_t *rl = lv_label_create(reset_btn);
    lv_label_set_text(rl, LV_SYMBOL_TRASH "  Reset WiFi / settings");
    lv_obj_center(rl);
    lv_obj_add_event_cb(reset_btn, reset_btn_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Tab switching ---------------------------------------------------------
static void tab_changed_cb(lv_event_t *e) {
    lv_obj_t *btns = lv_event_get_target(e);
    uint32_t idx = lv_tabview_get_tab_act(btns);
    if (idx == 0) ui_scenes_request_refresh();
    if (idx == 1) ui_virtuals_request_refresh();
    if (idx == 2) { ui_virtuals_request_refresh(); ui_global_status_tick(); }  // refresh globals
}

// ---- Init ------------------------------------------------------------------
void ui_init(void) {
    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0d0d14), 0);  // dark ground
    lv_scr_load(s_root);

    s_tabview = lv_tabview_create(s_root, LV_DIR_TOP, 40);
    s_tab_scenes   = lv_tabview_add_tab(s_tabview, "Scenes");
    s_tab_virtuals = lv_tabview_add_tab(s_tabview, "Virtuals");
    s_tab_global   = lv_tabview_add_tab(s_tabview, "Global");
    lv_obj_add_event_cb(s_tabview, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Status bar at the bottom
    s_status_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_status_label, 800);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_status_label, "Ready");

    // Per-tab UIs live in their own modules.
    ui_scenes_build(s_tab_scenes);
    ui_virtuals_build(s_tab_virtuals);
    build_global_screen();

    ui_show_status("Connecting to LedFx…");

    // Drain worker results (and repaint) on the LVGL thread.
    lv_timer_create(result_pump_cb, RESULT_PUMP_MS, NULL);
    // Kick off the first fetches (the worker connects on demand). Virtuals also
    // carries the global state that seeds the Global-screen controls.
    ui_scenes_request_refresh();
    ui_virtuals_request_refresh();
    // Keep the front data screen (and the status row) in sync with LedFx.
    lv_timer_create(auto_refresh_cb, AUTO_REFRESH_MS, NULL);

    // Boot splash + auto-dim timer (both live in ui_global because they share
    // the panel-backlight state with the Global-tab slider).
    ui_global_install_overlays();
}

void ui_show_status(const char *msg, bool is_error) {
    if (!s_status_label) return;
    lv_label_set_text(s_status_label, msg);
    lv_obj_set_style_text_color(s_status_label,
                                is_error ? lv_color_hex(0xff4444) : lv_color_hex(0xcccccc), 0);
}

// printf-style variant — avoids the temporary String allocation that
// ("Foo: " + name).c_str() would otherwise create on every event.
void ui_show_status_fmt(bool is_error, const char *fmt, ...) {
    static char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_show_status(buf, is_error);
}

void ui_refresh_scenes(void)   { ui_scenes_request_refresh(); }
void ui_refresh_virtuals(void) { ui_virtuals_request_refresh(); }

lv_obj_t *ui_root(void) { return s_root; }
