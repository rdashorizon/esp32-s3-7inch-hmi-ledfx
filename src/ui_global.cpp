// ---------------------------------------------------------------------------
// ui_global.cpp — Global tab + connection indicator + auto-dim + boot splash
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#include "ui_global.h"
#include "ui.h"           // ui_show_status, ui_submit (for the overlay)
#include "config.h"       // g_config, config_store, Theme/ThemeMode/AccentColor
#include "display.h"      // display_set_backlight
#include "worker.h"       // worker_submit, req_*, REQ_*
#include "ui_theme.h"     // ui_theme_*() palette accessors (Tier 1.4)
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
// Theme picker widget refs (Tier 1.4). Held as statics so apply_theme() can
// move the "currently selected" highlight after a tap.
static lv_obj_t *s_theme_box = nullptr;
static lv_obj_t *s_theme_mode_btn_dark = nullptr;
static lv_obj_t *s_theme_mode_btn_light = nullptr;
static lv_obj_t *s_theme_accent_btn[4] = {nullptr, nullptr, nullptr, nullptr};

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

// Sole owner of the indicator's color — it depends on BOTH link state and
// theme mode, so splitting it across two modules meant whichever ran last won.
// Green/amber are status colors rather than theme accents; only the green is
// darkened in Light mode, where the brighter one washes out.
static void update_conn_indicator(void) {
    if (!s_conn_dot) return;
    uint32_t connected = (ui_theme_current().mode == ThemeMode::LIGHT)
                             ? 0x22aa44 : 0x33cc66;
    lv_obj_set_style_text_color(s_conn_dot,
        lv_color_hex(s_link_ok ? connected : 0xd8a11a), 0);
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

    // ---- Theme picker (Tier 1.4) -----------------------------------------
    // Mode (Dark/Light) + Accent (Blue/Green/Orange/Magenta) presets.
    // Live preview: each tap calls ui_theme_set_and_apply() which persists
    // to NVS + restyles every visible widget. The internal 200 ms debounce
    // keeps rapid taps from causing paint thrash.
    s_theme_box = lv_obj_create(parent);
    // Height from content, not a fixed 170 px: two labels + two 40 px rows +
    // a 36 px button + padding and flex gaps overflow that, and the box is
    // non-scrollable, so the "Defaults" button was clipped off the bottom and
    // could not be tapped.
    lv_obj_set_size(s_theme_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_theme_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_theme_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_theme_box, 8, 0);
    lv_obj_set_style_bg_color(s_theme_box,
        lv_color_hex(ui_theme_panel_bg()), 0);
    lv_obj_set_style_border_color(s_theme_box,
        lv_color_hex(ui_theme_border()), 0);
    lv_obj_set_style_border_width(s_theme_box, 1, 0);
    lv_obj_set_style_radius(s_theme_box, 4, 0);

    lv_obj_t *mode_lbl = lv_label_create(s_theme_box);
    lv_label_set_text(mode_lbl, "Mode");
    lv_obj_set_style_text_color(mode_lbl,
        lv_color_hex(ui_theme_text_primary()), 0);

    lv_obj_t *mode_row = lv_obj_create(s_theme_box);
    lv_obj_set_size(mode_row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(mode_row, 8, 0);

    auto make_mode_btn = [&](const char *lbl_text, ThemeMode m,
                            lv_obj_t **btn_out) {
        lv_obj_t *b = lv_btn_create(mode_row);
        lv_obj_set_size(b, 100, 36);
        *btn_out = b;
        lv_obj_set_user_data(b, (void *)(intptr_t)m);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            lv_obj_t *btn = lv_event_get_target(e);
            ThemeMode m = (ThemeMode)(intptr_t)lv_obj_get_user_data(btn);
            Theme t = ui_theme_current();
            t.mode = m;
            // ui_theme_set_and_apply() dispatches back into this module's
            // apply_theme(), which moves the "selected" highlight.
            ui_theme_set_and_apply(t);
        }, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, lbl_text);
        lv_obj_center(l);
        return b;
    };
    make_mode_btn("Dark",  ThemeMode::DARK,  &s_theme_mode_btn_dark);
    make_mode_btn("Light", ThemeMode::LIGHT, &s_theme_mode_btn_light);

    lv_obj_t *accent_lbl = lv_label_create(s_theme_box);
    lv_label_set_text(accent_lbl, "Accent");
    lv_obj_set_style_text_color(accent_lbl,
        lv_color_hex(ui_theme_text_primary()), 0);

    lv_obj_t *accent_row = lv_obj_create(s_theme_box);
    lv_obj_set_size(accent_row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(accent_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(accent_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(accent_row, 8, 0);

    const AccentColor accent_order[4] = {
        AccentColor::BLUE, AccentColor::GREEN,
        AccentColor::ORANGE, AccentColor::MAGENTA,
    };
    const char *accent_lbls[4] = { "Blue", "Green", "Orange", "Magenta" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(accent_row);
        lv_obj_set_size(b, 90, 36);
        lv_obj_set_user_data(b, (void *)(intptr_t)accent_order[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, accent_lbls[i]);
        lv_obj_center(l);
        s_theme_accent_btn[i] = b;
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            lv_obj_t *btn = lv_event_get_target(e);
            AccentColor a = (AccentColor)(intptr_t)lv_obj_get_user_data(btn);
            Theme t = ui_theme_current();
            t.accent = a;
            ui_theme_set_and_apply(t);
        }, LV_EVENT_CLICKED, NULL);
    }

    // Reset to defaults (Tier 2). Reverts to Dark + Blue and triggers the
    // confirmation toast via ui_theme_set_and_apply().
    lv_obj_t *reset_btn = lv_btn_create(s_theme_box);
    lv_obj_set_size(reset_btn, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(reset_btn,
        lv_color_hex(ui_theme_panel_alt()), 0);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, "Defaults");
    lv_obj_center(reset_lbl);
    lv_obj_add_event_cb(reset_btn, [](lv_event_t *e) {
        (void)e;
        Theme t;
        t.mode = ThemeMode::DARK;
        t.accent = AccentColor::BLUE;
        ui_theme_set_and_apply(t);
    }, LV_EVENT_CLICKED, NULL);

    // Connection / status row
    s_gstatus_label = lv_label_create(parent);
    lv_obj_set_width(s_gstatus_label, LV_PCT(100));
    update_global_status();

    // Persistent WiFi link indicator on the top layer (always visible).
    s_conn_dot = lv_label_create(lv_layer_top());
    lv_label_set_text(s_conn_dot, LV_SYMBOL_WIFI);
    lv_obj_align(s_conn_dot, LV_ALIGN_TOP_RIGHT, -12, 10);
    update_conn_indicator();

    // First paint of everything theme-dependent (status row color, picker
    // panel, selected-button highlight) goes through the same hook the Theme
    // picker uses at runtime, so build and re-theme can't drift apart.
    ui_global_apply_theme();
}

// ---- Theme support (Tier 1.4) -------------------------------------------
// ui_theme.cpp dispatches here via ui_global::apply_theme(). Re-styles the
// status row + the picker highlight + the picker panel border. The conn-dot
// is restyled by ui_theme.cpp directly (it has the only cross-screen static).
static void restyle_picker_highlight(void) {
    Theme cur = ui_theme_current();
    // Mode buttons: highlight the active one.
    if (s_theme_mode_btn_dark) {
        lv_obj_set_style_bg_color(s_theme_mode_btn_dark,
            (cur.mode == ThemeMode::DARK)
                ? lv_color_hex(ui_theme_accent_rgb565())
                : lv_color_hex(ui_theme_panel_alt()), 0);
    }
    if (s_theme_mode_btn_light) {
        lv_obj_set_style_bg_color(s_theme_mode_btn_light,
            (cur.mode == ThemeMode::LIGHT)
                ? lv_color_hex(ui_theme_accent_rgb565())
                : lv_color_hex(ui_theme_panel_alt()), 0);
    }
    // Accent buttons: highlight the active one.
    const AccentColor accent_order[4] = {
        AccentColor::BLUE, AccentColor::GREEN,
        AccentColor::ORANGE, AccentColor::MAGENTA,
    };
    for (int i = 0; i < 4; i++) {
        if (s_theme_accent_btn[i]) {
            lv_obj_set_style_bg_color(s_theme_accent_btn[i],
                (cur.accent == accent_order[i])
                    ? lv_color_hex(ui_theme_accent_rgb565())
                    : lv_color_hex(ui_theme_panel_alt()), 0);
        }
    }
}

void ui_global_apply_theme(void) {
    // Status row (bottom of Global tab).
    if (s_gstatus_label) {
        lv_obj_set_style_text_color(s_gstatus_label,
            lv_color_hex(ui_theme_text_muted()), 0);
    }
    // Theme picker container — panel + border + the "currently selected"
    // button highlight.
    if (s_theme_box) {
        lv_obj_set_style_bg_color(s_theme_box,
            lv_color_hex(ui_theme_panel_bg()), 0);
        lv_obj_set_style_border_color(s_theme_box,
            lv_color_hex(ui_theme_border()), 0);
        restyle_picker_highlight();
    }
    // Depends on the theme mode as well as the link state.
    update_conn_indicator();
}

namespace ui_global {
    void apply_theme() { ui_global_apply_theme(); }
}

void ui_global_apply_state(const GlobalsState &g) {
    // pause_sw is owned by the Virtuals module (Tier 1.3) — for now we don't
    // touch it here; callers pass it via ui_global_apply_state_with_pause()
    // if they need the pause-toggle synced.
    if (!g.valid) return;
    // mirror/flip are only meaningful when an active effect supplied them.
    // Writing them unconditionally forced both switches off on every refresh
    // whenever nothing was active, contradicting the server.
    if (g.has_flags) {
        set_sw(s_mirror_sw, g.mirror);
        set_sw(s_flip_sw,   g.flip);
    }
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
