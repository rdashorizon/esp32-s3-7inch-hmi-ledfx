// ---------------------------------------------------------------------------
// ui_global.cpp — Global tab + connection indicator + auto-dim + boot splash
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#include "ui_global.h"
#include "ui.h"           // ui_show_status, ui_submit (for the overlay)
#include "config.h"       // g_config, config_store
#include "display.h"      // display_set_backlight
#include "worker.h"       // worker_submit, req_*, REQ_*
#include <ArduinoJson.h>  // StaticJsonDocument
#include <Arduino.h>      // millis, delay

extern Config g_config;  // defined in main.cpp

// ---- Module state (LVGL-thread-only, see THREADING OWNERSHIP in ui.cpp) ----
static lv_obj_t *s_bright_slider = nullptr;
static lv_obj_t *s_bright_value  = nullptr;
// Global-screen controls that get synced from server state on refresh.
static lv_obj_t *s_mirror_sw = nullptr;
static lv_obj_t *s_flip_sw   = nullptr;

// Connection / status row.
static lv_obj_t *s_gstatus_label = nullptr;
// Persistent WiFi indicator on the top layer (always visible).
static lv_obj_t *s_conn_dot = nullptr;

// Link state and last successful data refresh time. Written by the result pump
// (ui.cpp) via ui_global_set_link() and ui_global_mark_refreshed(); read here.
static bool     s_link_ok         = false;
static uint32_t s_last_refresh_ms = 0;

// Panel backlight (this device's LCD) — separate from LedFx LED brightness.
static lv_obj_t *s_screen_bright_slider = nullptr;
static int      s_screen_bright_pct = 100;  // persisted panel brightness (%)
static uint8_t  s_screen_bright     = 255;  // target backlight when awake (0..255)
static bool     s_dimmed            = false;
static const uint32_t DIM_TIMEOUT_MS = 60000;  // dim after 60 s with no touch
static const uint8_t  DIM_LEVEL      = 24;     // ~10 % while dimmed

// ---- Brightness callbacks --------------------------------------------------
static void bright_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(s_bright_value, "%d%%", v);
}

// Build an apply_global body and hand it to the worker.
static void submit_global(const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    ui_submit(req_payload(REQ_APPLY_GLOBAL, body));
}

static void bright_apply_cb(lv_event_t *e) {
    (void)e;
    // Drive LedFx's master global_brightness (config), so it matches the main
    // LedFx brightness and dims the whole installation.
    int v = lv_slider_get_value(s_bright_slider);
    ui_submit(req_arg(REQ_SET_BRIGHTNESS, v));
    ui_show_status("Applying brightness…");
}

static void mirror_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["mirror"] = lv_obj_has_state(sw, LV_STATE_CHECKED);
    submit_global(doc);
}

static void flip_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["flip"] = lv_obj_has_state(sw, LV_STATE_CHECKED);
    submit_global(doc);
}

static void gradient_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    char buf[32] = {0};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    ui_submit(req_payload(REQ_SET_GRADIENT, String(buf)));
    ui_show_status_fmt(false, "Gradient: %s", buf);
}

// ---- Status row + indicator ------------------------------------------------
// Repaint the Global screen status row: server URL, link state, last refresh.
// Link state comes from s_link_ok (set by the result pump) — never poll the
// network client here, that would race the worker thread.
static void update_global_status(void) {
    if (!s_gstatus_label) return;
    String s = "Server: ";
    s += g_config.ledfx_url.length() ? g_config.ledfx_url : String("(unset)");
    s += s_link_ok ? "\nLedFx: connected" : "\nLedFx: connecting…";
    if (s_last_refresh_ms) {
        s += "\nLast refresh: " + String((millis() - s_last_refresh_ms) / 1000) + "s ago";
    } else {
        s += "\nLast refresh: never";
    }
    // OTA target (Tier 4). Shown here so the user can read the network-update
    // hostname + password off the panel and reflash over WiFi without a serial
    // cable. LAN-only convenience; the AP setup form already prints the PSK the
    // same way, so this is consistent with the device's existing posture.
    s += "\nOTA: " + config_ota_hostname() + ".local  pw " + config_ota_password();
    lv_label_set_text(s_gstatus_label, s.c_str());
}

static void update_conn_indicator(void) {
    if (!s_conn_dot) return;
    lv_obj_set_style_text_color(s_conn_dot,
        s_link_ok ? lv_color_hex(0x33cc66) : lv_color_hex(0xd8a11a), 0);
}

// Reflect server global state onto the Global-screen controls. Programmatic
// state/value changes do not fire LV_EVENT_VALUE_CHANGED, so this can't loop
// back into the command handlers. `pause_sw` is created on the Virtuals tab
// but its checked-state mirrors the server's `paused` flag, so the caller
// (result pump, or anyone wiring the global state) passes it in.
static void set_sw(lv_obj_t *sw, bool on) {
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_clear_state(sw, LV_STATE_CHECKED);
}
static void apply_globals_to_ui(const GlobalsState &g, lv_obj_t *pause_sw) {
    if (!g.valid) return;
    set_sw(pause_sw, g.paused);
    if (g.has_flags) {
        set_sw(s_mirror_sw, g.mirror);
        set_sw(s_flip_sw, g.flip);
    }
    if (g.has_brightness) {
        if (s_bright_slider) lv_slider_set_value(s_bright_slider, g.brightness, LV_ANIM_OFF);
        if (s_bright_value)  lv_label_set_text_fmt(s_bright_value, "%d%%", g.brightness);
    }
}

// ---- Panel backlight slider + auto-dim -------------------------------------
static void screen_bright_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    s_screen_bright_pct = lv_slider_get_value(sl);   // 10..100
    s_screen_bright = (uint8_t)(s_screen_bright_pct * 255 / 100);
    if (!s_dimmed) display_set_backlight(s_screen_bright);
}

// Persist the panel brightness once the user finishes dragging (not on every
// tick — that would hammer NVS).
static void screen_bright_save_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    config_store.save_screen_brightness((uint8_t)lv_slider_get_value(sl));
}

// Dim the panel after a period of no touch (burn-in + power); restore on touch.
static void dim_tick_cb(lv_timer_t *t) {
    (void)t;
    uint32_t idle = lv_disp_get_inactive_time(NULL);
    if (idle > DIM_TIMEOUT_MS && !s_dimmed) {
        display_set_backlight(DIM_LEVEL);
        s_dimmed = true;
    } else if (idle <= DIM_TIMEOUT_MS && s_dimmed) {
        display_set_backlight(s_screen_bright);
        s_dimmed = false;
    }
}

// ---- Splash overlay --------------------------------------------------------
// Remove the one-shot boot splash overlay.
static void splash_done_cb(lv_timer_t *t) {
    lv_obj_t *splash = (lv_obj_t *)t->user_data;
    if (splash) lv_obj_del(splash);  // timer auto-deletes (repeat count reached 0)
}

static void install_splash(void) {
    lv_obj_t *splash = lv_obj_create(lv_layer_top());
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x0a0a12), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash, 0, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *stitle = lv_label_create(splash);
    lv_label_set_text(stitle, LV_SYMBOL_HOME "  LedFx HMI");
    lv_obj_center(stitle);
    // repeat_count=1 makes LVGL auto-delete this timer after the splash overlay
    // is removed (lv_timer_t::one_shot-style cleanup). Don't change it without
    // also handling timer cleanup explicitly — a forever-loop timer with
    // user_data pointing at a deleted splash object would crash on the next tick.
    lv_timer_t *st = lv_timer_create(splash_done_cb, 1200, splash);
    lv_timer_set_repeat_count(st, 1);
}

// ---- Public API ------------------------------------------------------------
void ui_global_build(lv_obj_t *parent) {
    // Restore the persisted panel brightness and apply it immediately. Done
    // here rather than in ui_init so the slider's restored value matches.
    s_screen_bright_pct = config_store.load_screen_brightness(100);
    s_screen_bright = (uint8_t)(s_screen_bright_pct * 255 / 100);
    display_set_backlight(s_screen_bright);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 16, 0);

    // Brightness row
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Brightness");
    s_bright_slider = lv_slider_create(row);
    lv_obj_set_width(s_bright_slider, 400);
    lv_slider_set_range(s_bright_slider, 0, 100);
    lv_slider_set_value(s_bright_slider, 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bright_slider, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_bright_value = lv_label_create(row);
    lv_label_set_text_fmt(s_bright_value, "%d%%", 80);

    lv_obj_t *apply = lv_btn_create(row);
    lv_obj_t *al = lv_label_create(apply);
    lv_label_set_text(al, "Apply");
    lv_obj_center(al);
    lv_obj_add_event_cb(apply, bright_apply_cb, LV_EVENT_CLICKED, NULL);

    // Mirror row
    lv_obj_t *mrow = lv_obj_create(parent);
    lv_obj_set_size(mrow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ml = lv_label_create(mrow);
    lv_label_set_text(ml, "Mirror");
    s_mirror_sw = lv_switch_create(mrow);
    lv_obj_add_event_cb(s_mirror_sw, mirror_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *frow = lv_obj_create(parent);
    lv_obj_set_size(frow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *fl = lv_label_create(frow);
    lv_label_set_text(fl, "Flip");
    s_flip_sw = lv_switch_create(frow);
    lv_obj_add_event_cb(s_flip_sw, flip_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Screen backlight (this panel, not the LEDs) — live, no Apply needed.
    lv_obj_t *brow = lv_obj_create(parent);
    lv_obj_set_size(brow, LV_PCT(100), 70);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sbl = lv_label_create(brow);
    lv_label_set_text(sbl, "Screen brightness");
    s_screen_bright_slider = lv_slider_create(brow);
    lv_obj_set_width(s_screen_bright_slider, 400);
    lv_slider_set_range(s_screen_bright_slider, 10, 100);
    lv_slider_set_value(s_screen_bright_slider, s_screen_bright_pct, LV_ANIM_OFF);  // restored
    lv_obj_add_event_cb(s_screen_bright_slider, screen_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_screen_bright_slider, screen_bright_save_cb, LV_EVENT_RELEASED, NULL);

    // Gradient preset picker
    lv_obj_t *grow = lv_obj_create(parent);
    lv_obj_set_size(grow, LV_PCT(100), 70);
    lv_obj_set_flex_flow(grow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *gl = lv_label_create(grow);
    lv_label_set_text(gl, "Gradient");
    lv_obj_t *dd = lv_dropdown_create(grow);
    lv_obj_set_width(dd, 300);
    // LedFx's built-in gradient keys (ledfx/color.py LEDFX_GRADIENTS). These
    // must match exactly — LedFx rejects unknown names.
    lv_dropdown_set_options(dd,
        "Rainbow\nDancefloor\nPlasma\nOcean\nViridis\nJungle\n"
        "Spring\nWinter\nFrost\nSunset\nBorealis\nRust\nWinamp");
    lv_obj_add_event_cb(dd, gradient_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Connection / status row
    s_gstatus_label = lv_label_create(parent);
    lv_obj_set_width(s_gstatus_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_gstatus_label, lv_color_hex(0xaaaaaa), 0);
    update_global_status();

    // Persistent WiFi link indicator on the top layer (always visible).
    s_conn_dot = lv_label_create(lv_layer_top());
    lv_label_set_text(s_conn_dot, LV_SYMBOL_WIFI);
    lv_obj_align(s_conn_dot, LV_ALIGN_TOP_RIGHT, -12, 10);
    update_conn_indicator();
}

void ui_global_apply_state(const GlobalsState &g) {
    // pause_sw is owned by the Virtuals module (Tier 1.3) — for now we don't
    // touch it here; callers pass it via ui_global_apply_state_with_pause()
    // if they need the pause-toggle synced.
    set_sw(s_mirror_sw, g.mirror);
    set_sw(s_flip_sw,   g.flip);
    if (g.has_brightness) {
        if (s_bright_slider) lv_slider_set_value(s_bright_slider, g.brightness, LV_ANIM_OFF);
        if (s_bright_value)  lv_label_set_text_fmt(s_bright_value, "%d%%", g.brightness);
    }
}

// Variant that also drives the Virtuals-tab pause switch. Used by the result
// pump when it knows which switch belongs to the Virtuals toolbar.
void ui_global_apply_state_with_pause(const GlobalsState &g, lv_obj_t *pause_sw) {
    if (g.valid) set_sw(pause_sw, g.paused);
    ui_global_apply_state(g);
}

void ui_global_set_link(bool connected) {
    s_link_ok = connected;
    update_global_status();
    update_conn_indicator();
}

bool ui_global_link_ok(void) { return s_link_ok; }

void ui_global_mark_refreshed(void) {
    s_last_refresh_ms = millis();
    update_global_status();
}

void ui_global_status_tick(void) {
    update_global_status();
}

void ui_global_install_overlays(void) {
    install_splash();
    // Auto-dim the panel after a period of no touch.
    lv_timer_create(dim_tick_cb, 1000, NULL);
}
