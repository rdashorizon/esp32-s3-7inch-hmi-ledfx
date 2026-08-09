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
#include "ui.h"
#include "ledfx.h"     // SceneInfo / VirtualInfo
#include "worker.h"    // background network worker (submit/poll)
#include "config.h"
#include "display.h"   // display_set_backlight()
#include <ArduinoJson.h>

extern Config g_config;  // see main.cpp

static lv_obj_t *s_root;
static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_scenes;
static lv_obj_t *s_tab_virtuals;
static lv_obj_t *s_tab_global;
static lv_obj_t *s_status_label;

// Global-screen connection/status row + last successful data refresh.
static lv_obj_t *s_gstatus_label = nullptr;
static uint32_t  s_last_refresh_ms = 0;
// Link state, mirrored from the worker's RES_CONN messages. The UI never reads
// the network client directly (that would race the worker thread).
static bool      s_link_ok = false;

// Per-data-screen loading spinner + error banner (overlaid on the tab).
static lv_obj_t *s_scene_spinner = nullptr;
static lv_obj_t *s_scene_error   = nullptr;
static lv_obj_t *s_virt_spinner  = nullptr;
static lv_obj_t *s_virt_error    = nullptr;

// Persistent link indicator (top layer, always visible).
static lv_obj_t *s_conn_dot = nullptr;

// On-device screen backlight + auto-dim on inactivity.
static lv_obj_t *s_screen_bright_slider = nullptr;
static int     s_screen_bright_pct = 100;  // persisted panel brightness (%)
static uint8_t s_screen_bright = 255;      // target backlight when awake (0..255)
static bool    s_dimmed        = false;
static const uint32_t DIM_TIMEOUT_MS = 60000;  // dim after 60 s with no touch
static const uint8_t  DIM_LEVEL      = 24;     // ~10 % while dimmed

// Refresh the active data screen (scenes/virtuals) on a timer so the HUD
// tracks LedFx state changes made elsewhere. Period in milliseconds.
static const uint32_t AUTO_REFRESH_MS = 30000;
// How often the UI drains network results from the worker and repaints.
static const uint32_t RESULT_PUMP_MS = 80;

static void update_global_status(void);
static void request_scenes(void);
static void request_virtuals(void);

// Show/hide an optional widget (spinner, error banner).
static void obj_show(lv_obj_t *o, bool show) {
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- Scenes screen ---------------------------------------------------------
static lv_obj_t *s_scene_grid;
static SceneInfo *s_scenes = nullptr;
static int s_scene_count = 0;

// 4-column grid; the row tracks are sized per render (below) so any scene count
// lays out and the grid scrolls when it's taller than the screen.
static lv_coord_t s_scene_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                       LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static lv_coord_t s_scene_row_dsc[10] = {LV_GRID_TEMPLATE_LAST};  // filled per render

static void scene_btn_clicked(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    worker_submit(req_id(REQ_ACTIVATE_SCENE, s_scenes[idx].id));
    ui_show_status(("Activating: " + s_scenes[idx].name).c_str());
}

static void scene_btn_long(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    worker_submit(req_id(REQ_DEACTIVATE_SCENE, s_scenes[idx].id));
    ui_show_status(("Deactivating: " + s_scenes[idx].name).c_str());
}

// Ask the worker for a fresh scene list; render_scene_grid() paints the reply.
static void request_scenes(void) {
    if (worker_submit(req_simple(REQ_FETCH_SCENES))) {
        obj_show(s_scene_spinner, true);
        obj_show(s_scene_error, false);
        ui_show_status("Refreshing scenes…");
    }
}

// Paint the grid from the already-populated s_scenes[] (owned by the pump).
static void render_scene_grid(void) {
    lv_obj_clean(s_scene_grid);

    // Size the grid to exactly the rows we need (4 per row, cap 32 -> 8 rows)
    // so it scrolls vertically once it's taller than the tab.
    int rows = (s_scene_count + 3) / 4;
    if (rows < 1) rows = 1;
    if (rows > 8) rows = 8;
    for (int r = 0; r < rows; r++) s_scene_row_dsc[r] = 80;
    s_scene_row_dsc[rows] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(s_scene_grid, s_scene_col_dsc, s_scene_row_dsc);

    for (int i = 0; i < s_scene_count; i++) {
        const int col = i % 4;
        const int row = i / 4;
        lv_obj_t *btn = lv_btn_create(s_scene_grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                              LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, scene_btn_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, scene_btn_long, LV_EVENT_LONG_PRESSED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_scenes[i].name.c_str());
        lv_obj_center(lbl);

        if (s_scenes[i].active) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2266cc), 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x8ab4ff), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
        }
    }
    s_last_refresh_ms = millis();
    update_global_status();
    ui_show_status(s_scene_count ? "Scenes refreshed" : "No scenes found");
}

// ---- Virtuals screen -------------------------------------------------------
static lv_obj_t *s_virt_list;
static VirtualInfo *s_virt = nullptr;
static int s_virt_count = 0;

static void virt_toggle(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(sw);
    if (idx < 0 || idx >= s_virt_count) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    worker_submit(req_id(REQ_SET_VIRTUAL_ACTIVE, s_virt[idx].id, on));
}

static void virt_randomize(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_virt_count) return;
    worker_submit(req_id(REQ_RANDOMIZE_VIRTUAL, s_virt[idx].id));
    ui_show_status(("Randomizing: " + s_virt[idx].name).c_str());
}

static void clear_all_cb(lv_event_t *e) {
    (void)e;
    worker_submit(req_simple(REQ_CLEAR_ALL));
}

static void pause_all_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool paused = lv_obj_has_state(sw, LV_STATE_CHECKED);
    worker_submit(req_id(REQ_PAUSE_ALL, String(), paused));
}

// Ask the worker for a fresh virtual list; render_virt_list() paints the reply.
static void request_virtuals(void) {
    if (worker_submit(req_simple(REQ_FETCH_VIRTUALS))) {
        obj_show(s_virt_spinner, true);
        obj_show(s_virt_error, false);
        ui_show_status("Refreshing virtuals…");
    }
}

// Paint the list from the already-populated s_virt[] (owned by the pump).
static void render_virt_list(void) {
    lv_obj_clean(s_virt_list);

    for (int i = 0; i < s_virt_count; i++) {
        lv_obj_t *row = lv_obj_create(s_virt_list);
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // Subtitle: effect type, plus the effect's display name when it adds info.
        String sub;
        if (s_virt[i].effect_type.isEmpty()) {
            sub = "— no effect —";
        } else {
            sub = s_virt[i].effect_type;
            if (!s_virt[i].effect_name.isEmpty() && s_virt[i].effect_name != s_virt[i].effect_type)
                sub += " · " + s_virt[i].effect_name;
        }
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s\n%s", s_virt[i].name.c_str(), sub.c_str());

        lv_obj_t *dice = lv_btn_create(row);
        lv_obj_set_size(dice, 50, 50);
        lv_obj_t *dl = lv_label_create(dice);
        lv_label_set_text(dl, LV_SYMBOL_SHUFFLE);
        lv_obj_center(dl);
        lv_obj_set_user_data(dice, (void *)(intptr_t)i);
        lv_obj_add_event_cb(dice, virt_randomize, LV_EVENT_CLICKED, NULL);

        lv_obj_t *sw = lv_switch_create(row);
        if (s_virt[i].active) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_user_data(sw, (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, virt_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (s_virt_count == 0) {
        lv_obj_t *empty = lv_label_create(s_virt_list);
        lv_label_set_text(empty, "No virtuals configured yet.");
    }
    s_last_refresh_ms = millis();
    update_global_status();
    ui_show_status("Virtuals refreshed");
}

// ---- Global screen ---------------------------------------------------------
static lv_obj_t *s_bright_slider;
static lv_obj_t *s_bright_value;
// Global-screen controls that get synced from server state on refresh.
static lv_obj_t *s_mirror_sw = nullptr;
static lv_obj_t *s_flip_sw   = nullptr;
static lv_obj_t *s_pause_sw  = nullptr;

static void bright_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(s_bright_value, "%d%%", v);
}

// Build an apply_global body and hand it to the worker.
static void submit_global(const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    worker_submit(req_payload(REQ_APPLY_GLOBAL, body));
}

static void bright_apply_cb(lv_event_t *e) {
    (void)e;
    // Drive LedFx's master global_brightness (config), so it matches the main
    // LedFx brightness and dims the whole installation.
    int v = lv_slider_get_value(s_bright_slider);
    worker_submit(req_arg(REQ_SET_BRIGHTNESS, v));
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
    worker_submit(req_payload(REQ_SET_GRADIENT, String(buf)));
    ui_show_status((String("Gradient: ") + buf).c_str());
}

// Repaint the Global screen status row: server URL, link state, last refresh.
// Link state comes from s_link_ok (set by the worker) — never poll the network
// client here, that would race the worker thread.
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
    lv_label_set_text(s_gstatus_label, s.c_str());
}

// Persistent WiFi indicator on the top layer: green when connected, amber while
// connecting. Driven by s_link_ok (updated from RES_CONN).
static void update_conn_indicator(void) {
    if (!s_conn_dot) return;
    lv_obj_set_style_text_color(s_conn_dot,
        s_link_ok ? lv_color_hex(0x33cc66) : lv_color_hex(0xd8a11a), 0);
}

// Reflect server global state onto the Global-screen controls. Programmatic
// state/value changes do not fire LV_EVENT_VALUE_CHANGED, so this can't loop
// back into the command handlers.
static void set_sw(lv_obj_t *sw, bool on) {
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_clear_state(sw, LV_STATE_CHECKED);
}
static void apply_globals_to_ui(const GlobalsState &g) {
    if (!g.valid) return;
    set_sw(s_pause_sw, g.paused);
    if (g.has_flags) {
        set_sw(s_mirror_sw, g.mirror);
        set_sw(s_flip_sw, g.flip);
    }
    if (g.has_brightness) {
        if (s_bright_slider) lv_slider_set_value(s_bright_slider, g.brightness, LV_ANIM_OFF);
        if (s_bright_value)  lv_label_set_text_fmt(s_bright_value, "%d%%", g.brightness);
    }
}

// Enqueue a refresh for whichever data screen is currently in front.
static void request_active_screen(void) {
    uint32_t idx = lv_tabview_get_tab_act(s_tabview);
    if (idx == 0) request_scenes();
    else if (idx == 1) request_virtuals();
}

// Periodic refresh of the front data screen, plus a status tick.
static void auto_refresh_cb(lv_timer_t *t) {
    (void)t;
    update_global_status();
    if (s_link_ok) request_active_screen();
}

// Drain the worker's result queue on the LVGL thread and repaint. This is the
// only place scene/virtual data or link state is adopted into the UI.
static void result_pump_cb(lv_timer_t *t) {
    (void)t;
    Result r;
    while (worker_poll(r)) {
        switch (r.type) {
            case RES_SCENES:
                delete[] s_scenes;  // release the previous list
                s_scenes = static_cast<SceneInfo *>(r.data);
                s_scene_count = r.count;
                obj_show(s_scene_spinner, false);
                if (r.status == 200) {
                    obj_show(s_scene_error, false);
                    render_scene_grid();
                } else {
                    lv_obj_clean(s_scene_grid);
                    if (s_scene_error)
                        lv_label_set_text(s_scene_error,
                            (String(LV_SYMBOL_WARNING "  ") + (r.msg[0] ? r.msg : "Couldn't reach LedFx")).c_str());
                    obj_show(s_scene_error, true);
                    ui_show_status(r.msg[0] ? r.msg : "Failed to fetch scenes", true);
                }
                break;
            case RES_VIRTUALS:
                delete[] s_virt;
                s_virt = static_cast<VirtualInfo *>(r.data);
                s_virt_count = r.count;
                obj_show(s_virt_spinner, false);
                if (r.status == 200) {
                    obj_show(s_virt_error, false);
                    render_virt_list();
                    apply_globals_to_ui(r.globals);  // sync Global-screen controls
                } else {
                    lv_obj_clean(s_virt_list);
                    if (s_virt_error)
                        lv_label_set_text(s_virt_error,
                            (String(LV_SYMBOL_WARNING "  ") + (r.msg[0] ? r.msg : "Couldn't reach LedFx")).c_str());
                    obj_show(s_virt_error, true);
                    ui_show_status(r.msg[0] ? r.msg : "Failed to fetch virtuals", true);
                }
                break;
            case RES_ACTION:
                ui_show_status(r.msg, r.status != 200);
                break;
            case RES_CONN:
                s_link_ok = r.connected;
                ui_show_status(r.msg, !r.connected);
                update_global_status();
                update_conn_indicator();
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

// On-device panel backlight slider (separate from LedFx LED brightness).
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

// Remove the one-shot boot splash overlay.
static void splash_done_cb(lv_timer_t *t) {
    lv_obj_t *splash = (lv_obj_t *)t->user_data;
    if (splash) lv_obj_del(splash);  // timer auto-deletes (repeat count reached 0)
}

// ---- Settings editor -------------------------------------------------------
// Edit WiFi/LedFx settings in place (no captive-portal round trip). Opens as a
// separate screen with an on-screen keyboard; Save persists to NVS and reboots.
static lv_obj_t *s_settings_scr = nullptr;
static lv_obj_t *s_ta_ssid = nullptr, *s_ta_wpass = nullptr, *s_ta_url = nullptr;
static lv_obj_t *s_ta_user = nullptr, *s_ta_lpass = nullptr, *s_settings_kb = nullptr;

static void settings_ta_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_settings_kb, ta);
        lv_obj_clear_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_kb_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
}

static void settings_close(void) {
    lv_scr_load(s_root);
    if (s_settings_scr) { lv_obj_del(s_settings_scr); s_settings_scr = nullptr; }
}
static void settings_back_cb(lv_event_t *e) { (void)e; settings_close(); }

static void settings_save_cb(lv_event_t *e) {
    (void)e;
    Config cfg;
    cfg.wifi_ssid  = lv_textarea_get_text(s_ta_ssid);
    cfg.wifi_pass  = lv_textarea_get_text(s_ta_wpass);
    cfg.ledfx_url  = lv_textarea_get_text(s_ta_url);
    cfg.ledfx_user = lv_textarea_get_text(s_ta_user);
    cfg.ledfx_pass = lv_textarea_get_text(s_ta_lpass);
    config_store.save(cfg);
    ui_show_status("Settings saved — rebooting…");
    lv_refr_now(NULL);
    delay(600);
    ESP.restart();
}

static lv_obj_t *settings_field(lv_obj_t *parent, const char *label,
                                const String &value, bool password) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, value.c_str());
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_ALL, NULL);
    return ta;
}

static void settings_open(void) {
    s_settings_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_scr, lv_color_hex(0x0d0d14), 0);
    lv_obj_clear_flag(s_settings_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header: Back / title / Save
    lv_obj_t *hdr = lv_obj_create(s_settings_scr);
    lv_obj_set_size(hdr, LV_PCT(100), 56);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " Back"); lv_obj_center(bl);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ttl = lv_label_create(hdr); lv_label_set_text(ttl, "Settings");
    lv_obj_t *save = lv_btn_create(hdr);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2266cc), 0);
    lv_obj_t *svl = lv_label_create(save); lv_label_set_text(svl, LV_SYMBOL_SAVE " Save"); lv_obj_center(svl);
    lv_obj_add_event_cb(save, settings_save_cb, LV_EVENT_CLICKED, NULL);

    // Scrollable form (sits above the keyboard when it appears)
    lv_obj_t *form = lv_obj_create(s_settings_scr);
    lv_obj_set_size(form, LV_PCT(100), 400);
    lv_obj_align(form, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    s_ta_ssid  = settings_field(form, "WiFi SSID", g_config.wifi_ssid, false);
    s_ta_wpass = settings_field(form, "WiFi password", g_config.wifi_pass, true);
    s_ta_url   = settings_field(form, "LedFx URL", g_config.ledfx_url, false);
    s_ta_user  = settings_field(form, "LedFx user (optional)", g_config.ledfx_user, false);
    s_ta_lpass = settings_field(form, "LedFx password (optional)", g_config.ledfx_pass, true);

    // On-screen keyboard, hidden until a field is focused.
    s_settings_kb = lv_keyboard_create(s_settings_scr);
    lv_obj_add_flag(s_settings_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_settings_kb, settings_kb_event, LV_EVENT_ALL, NULL);

    lv_scr_load(s_settings_scr);
}
static void settings_btn_cb(lv_event_t *e) { (void)e; settings_open(); }

static void build_global_screen(void) {
    lv_obj_set_flex_flow(s_tab_global, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_tab_global, 16, 0);

    // Brightness row
    lv_obj_t *row = lv_obj_create(s_tab_global);
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
    lv_obj_t *mrow = lv_obj_create(s_tab_global);
    lv_obj_set_size(mrow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ml = lv_label_create(mrow);
    lv_label_set_text(ml, "Mirror");
    s_mirror_sw = lv_switch_create(mrow);
    lv_obj_add_event_cb(s_mirror_sw, mirror_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *frow = lv_obj_create(s_tab_global);
    lv_obj_set_size(frow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *fl = lv_label_create(frow);
    lv_label_set_text(fl, "Flip");
    s_flip_sw = lv_switch_create(frow);
    lv_obj_add_event_cb(s_flip_sw, flip_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Screen backlight (this panel, not the LEDs) — live, no Apply needed.
    lv_obj_t *brow = lv_obj_create(s_tab_global);
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
    lv_obj_t *grow = lv_obj_create(s_tab_global);
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
    s_gstatus_label = lv_label_create(s_tab_global);
    lv_obj_set_width(s_gstatus_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_gstatus_label, lv_color_hex(0xaaaaaa), 0);
    update_global_status();

    // Settings + reset buttons on one row.
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
    lv_obj_add_event_cb(settings_btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);

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
    if (idx == 0) request_scenes();
    if (idx == 1) request_virtuals();
    if (idx == 2) { request_virtuals(); update_global_status(); }  // refresh globals
}

// ---- Init ------------------------------------------------------------------
void ui_init(void) {
    // Restore the persisted panel brightness and apply it immediately.
    s_screen_bright_pct = config_store.load_screen_brightness(100);
    s_screen_bright = (uint8_t)(s_screen_bright_pct * 255 / 100);
    display_set_backlight(s_screen_bright);

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

    // Scenes: 4-column grid, rows sized per render so it scrolls for any count.
    s_scene_grid = lv_obj_create(s_tab_scenes);
    lv_obj_set_size(s_scene_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(s_scene_grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(s_scene_grid, s_scene_col_dsc, s_scene_row_dsc);

    // Loading spinner + error banner overlaid on the scenes tab.
    s_scene_spinner = lv_spinner_create(s_tab_scenes, 1000, 60);
    lv_obj_set_size(s_scene_spinner, 56, 56);
    lv_obj_center(s_scene_spinner);
    lv_obj_add_flag(s_scene_spinner, LV_OBJ_FLAG_HIDDEN);
    s_scene_error = lv_label_create(s_tab_scenes);
    lv_obj_set_style_text_color(s_scene_error, lv_color_hex(0xff6666), 0);
    lv_obj_center(s_scene_error);
    lv_obj_add_flag(s_scene_error, LV_OBJ_FLAG_HIDDEN);

    s_virt_list = lv_obj_create(s_tab_virtuals);
    lv_obj_set_size(s_virt_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_virt_list, LV_FLEX_FLOW_COLUMN);

    // Loading spinner + error banner overlaid on the virtuals tab.
    s_virt_spinner = lv_spinner_create(s_tab_virtuals, 1000, 60);
    lv_obj_set_size(s_virt_spinner, 56, 56);
    lv_obj_center(s_virt_spinner);
    lv_obj_add_flag(s_virt_spinner, LV_OBJ_FLAG_HIDDEN);
    s_virt_error = lv_label_create(s_tab_virtuals);
    lv_obj_set_style_text_color(s_virt_error, lv_color_hex(0xff6666), 0);
    lv_obj_center(s_virt_error);
    lv_obj_add_flag(s_virt_error, LV_OBJ_FLAG_HIDDEN);

    // Clear-all button on virtuals screen
    lv_obj_t *clear_btn = lv_btn_create(s_tab_virtuals);
    lv_obj_set_size(clear_btn, 200, 50);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_t *cbl = lv_label_create(clear_btn);
    lv_label_set_text(cbl, "Clear all effects");
    lv_obj_center(cbl);
    lv_obj_add_event_cb(clear_btn, clear_all_cb, LV_EVENT_CLICKED, NULL);

    // Pause-all toggle on virtuals screen (bottom-left toolbar)
    lv_obj_t *pause_wrap = lv_obj_create(s_tab_virtuals);
    lv_obj_set_size(pause_wrap, 200, 50);
    lv_obj_align(pause_wrap, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_flex_flow(pause_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pause_wrap, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pl = lv_label_create(pause_wrap);
    lv_label_set_text(pl, "Pause all");
    s_pause_sw = lv_switch_create(pause_wrap);
    lv_obj_add_event_cb(s_pause_sw, pause_all_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_global_screen();

    // Persistent WiFi link indicator on the top layer (always visible).
    s_conn_dot = lv_label_create(lv_layer_top());
    lv_label_set_text(s_conn_dot, LV_SYMBOL_WIFI);
    lv_obj_align(s_conn_dot, LV_ALIGN_TOP_RIGHT, -12, 10);
    update_conn_indicator();

    ui_show_status("Connecting to LedFx…");

    // Drain worker results (and repaint) on the LVGL thread.
    lv_timer_create(result_pump_cb, RESULT_PUMP_MS, NULL);
    // Kick off the first fetches (the worker connects on demand). Virtuals also
    // carries the global state that seeds the Global-screen controls.
    request_scenes();
    request_virtuals();
    // Keep the front data screen (and the status row) in sync with LedFx.
    lv_timer_create(auto_refresh_cb, AUTO_REFRESH_MS, NULL);
    // Auto-dim the panel after a period of no touch.
    lv_timer_create(dim_tick_cb, 1000, NULL);

    // One-shot boot splash over everything, removed after ~1.2 s.
    lv_obj_t *splash = lv_obj_create(lv_layer_top());
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x0a0a12), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash, 0, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *stitle = lv_label_create(splash);
    lv_label_set_text(stitle, LV_SYMBOL_HOME "  LedFx HMI");
    lv_obj_center(stitle);
    lv_timer_t *st = lv_timer_create(splash_done_cb, 1200, splash);
    lv_timer_set_repeat_count(st, 1);
}

void ui_show_status(const char *msg, bool is_error) {
    if (!s_status_label) return;
    lv_label_set_text(s_status_label, msg);
    lv_obj_set_style_text_color(s_status_label,
                                is_error ? lv_color_hex(0xff4444) : lv_color_hex(0xcccccc), 0);
}

void ui_refresh_scenes(void)   { request_scenes(); }
void ui_refresh_virtuals(void) { request_virtuals(); }
