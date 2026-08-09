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
// All UI state below is owned by the LVGL thread (core 1):
//   - s_scenes / s_scene_count — written only by result_pump_cb (RES_SCENES)
//   - s_virt   / s_virt_count   — written only by result_pump_cb (RES_VIRTUALS)
//   - s_pause_sw                — created in ui_init, toggled by Virtuals
//                                 toolbar; state is synced from server via
//                                 ui_global_apply_state_with_pause().
// The worker's link state and "last refresh" timestamp live in ui_global
// (ui_global_link_ok(), ui_global_mark_refreshed()) — same LVGL thread, no
// race. The worker (core 0) MUST NOT touch any UI state directly; it goes
// through worker_submit() and posts a Result. If you need a new piece of
// cross-thread state, add it to the Result struct in worker.h and read it
// in the pump.
// ---------------------------------------------------------------------------
#include "ui.h"
#include "ledfx.h"     // SceneInfo / VirtualInfo
#include "worker.h"    // background network worker (submit/poll)
#include "config.h"
#include "display.h"   // display_set_backlight()
#include "ui_settings.h"  // on-device settings editor (extracted module)
#include "ui_global.h"    // Global tab + dim + splash + conn indicator
#include <ArduinoJson.h>

extern Config g_config;  // see main.cpp

static lv_obj_t *s_root;
static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_scenes;
static lv_obj_t *s_tab_virtuals;
static lv_obj_t *s_tab_global;
static lv_obj_t *s_status_label;

// s_pause_sw lives here (created on the Virtuals tab in ui_init) because
// it crosses tab boundaries: the Virtuals toolbar toggles it, and the
// result pump's RES_VIRTUALS handler (which lives here) syncs its state
// from the server via ui_global_apply_state_with_pause().
static lv_obj_t *s_pause_sw = nullptr;

// Per-data-screen loading spinner + error banner (overlaid on the tab).
static lv_obj_t *s_scene_spinner = nullptr;
static lv_obj_t *s_scene_error   = nullptr;
static lv_obj_t *s_virt_spinner  = nullptr;
static lv_obj_t *s_virt_error    = nullptr;

// Refresh the active data screen (scenes/virtuals) on a timer so the HUD
// tracks LedFx state changes made elsewhere. Period in milliseconds.
static const uint32_t AUTO_REFRESH_MS = 30000;
// How often the UI drains network results from the worker and repaints.
static const uint32_t RESULT_PUMP_MS = 80;

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
    ui_show_status(s_scene_count ? "Scenes refreshed" : "No scenes found");
    ui_global_mark_refreshed();
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
    worker_submit(req_flag(REQ_PAUSE_ALL, paused));
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
    ui_global_mark_refreshed();
    ui_show_status("Virtuals refreshed");
}

// ---- Global screen ---------------------------------------------------------
// The Global tab is built and managed by ui_global.{h,cpp}. This file just
// owns s_pause_sw (created on the Virtuals tab in ui_init; the checked-state
// is driven by ui_global_apply_state_with_pause() from the result pump) and
// the reset-button handler (which lives here because it touches NVS).

// Enqueue a refresh for whichever data screen is currently in front.
static void request_active_screen(void) {
    uint32_t idx = lv_tabview_get_tab_act(s_tabview);
    if (idx == 0) request_scenes();
    else if (idx == 1) request_virtuals();
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
                    ui_global_apply_state_with_pause(r.globals, s_pause_sw);
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
    if (idx == 0) request_scenes();
    if (idx == 1) request_virtuals();
    if (idx == 2) { request_virtuals(); ui_global_status_tick(); }  // refresh globals
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

    ui_show_status("Connecting to LedFx…");

    // Drain worker results (and repaint) on the LVGL thread.
    lv_timer_create(result_pump_cb, RESULT_PUMP_MS, NULL);
    // Kick off the first fetches (the worker connects on demand). Virtuals also
    // carries the global state that seeds the Global-screen controls.
    request_scenes();
    request_virtuals();
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

void ui_refresh_scenes(void)   { request_scenes(); }
void ui_refresh_virtuals(void) { request_virtuals(); }

lv_obj_t *ui_root(void) { return s_root; }
