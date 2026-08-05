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
#include "ledfx.h"
#include "config.h"
#include <ArduinoJson.h>

extern Net net;
extern ConfigStore config_store;
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

// Refresh the active data screen (scenes/virtuals) on a timer so the HUD
// tracks LedFx state changes made elsewhere. Period in milliseconds.
static const uint32_t AUTO_REFRESH_MS = 30000;

static void update_global_status(void);

// ---- Scenes screen ---------------------------------------------------------
static lv_obj_t *s_scene_grid;
static SceneInfo *s_scenes = nullptr;
static int s_scene_count = 0;

static void scene_btn_clicked(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    g_ledfx.activate_scene(s_scenes[idx].id);
    ui_show_status(("Activated: " + s_scenes[idx].name).c_str());
    ui_refresh_scenes();  // refresh active state
}

static void scene_btn_long(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    g_ledfx.deactivate_scene(s_scenes[idx].id);
    ui_show_status(("Deactivated: " + s_scenes[idx].name).c_str());
    ui_refresh_scenes();
}

static void rebuild_scene_grid(void) {
    lv_obj_clean(s_scene_grid);
    if (s_scenes) free(s_scenes);
    s_scenes = nullptr;
    s_scene_count = 0;

    int code = g_ledfx.fetch_scenes(s_scenes, s_scene_count);
    if (code != 200) {
        ui_show_status("Failed to fetch scenes", true);
        return;
    }

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
        }
    }
    s_last_refresh_ms = millis();
    update_global_status();
    ui_show_status("Scenes refreshed");
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
    g_ledfx.set_virtual_active(s_virt[idx].id, on);
}

static void virt_randomize(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_virt_count) return;
    g_ledfx.randomize_virtual(s_virt[idx].id);
    ui_show_status(("Randomized: " + s_virt[idx].name).c_str());
}

static void clear_all_cb(lv_event_t *e) {
    (void)e;
    g_ledfx.clear_all_effects();
    ui_show_status("Cleared all effects");
}

static void pause_all_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool paused = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_ledfx.pause_all(paused);
    ui_show_status(paused ? "Paused all virtuals" : "Resumed all virtuals");
}

static void rebuild_virt_list(void) {
    lv_obj_clean(s_virt_list);
    if (s_virt) free(s_virt);
    s_virt = nullptr;
    s_virt_count = 0;

    int code = g_ledfx.fetch_virtuals(s_virt, s_virt_count);
    if (code != 200) {
        ui_show_status("Failed to fetch virtuals", true);
        return;
    }

    for (int i = 0; i < s_virt_count; i++) {
        lv_obj_t *row = lv_obj_create(s_virt_list);
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s\n%s",
                              s_virt[i].name.c_str(),
                              s_virt[i].effect_type.isEmpty() ? "— no effect —" : s_virt[i].effect_type.c_str());

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

static void bright_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(s_bright_value, "%d%%", v);
}

static void bright_apply_cb(lv_event_t *e) {
    (void)e;
    int v = lv_slider_get_value(s_bright_slider);
    float f = v / 100.0f;
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["brightness"] = f;
    String body;
    serializeJson(doc, body);
    g_ledfx.apply_global(body);
    ui_show_status("Brightness applied");
}

static void mirror_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["mirror"] = on;
    String body;
    serializeJson(doc, body);
    g_ledfx.apply_global(body);
}

static void flip_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    StaticJsonDocument<128> doc;
    doc["action"] = "apply_global";
    doc["flip"] = on;
    String body;
    serializeJson(doc, body);
    g_ledfx.apply_global(body);
}

static void gradient_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    char buf[32] = {0};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    g_ledfx.set_gradient(String(buf));
    ui_show_status((String("Gradient: ") + buf).c_str());
}

// Repaint the Global screen status row: server URL, link state, last refresh.
static void update_global_status(void) {
    if (!s_gstatus_label) return;
    String s = "Server: ";
    s += g_config.ledfx_url.length() ? g_config.ledfx_url : String("(unset)");
    s += net.wifi_connected() ? "\nWiFi: connected" : "\nWiFi: down";
    s += g_ledfx.is_connected() ? "  |  LedFx: authenticated"
                                : "  |  LedFx: no token";
    if (s_last_refresh_ms) {
        s += "\nLast refresh: " + String((millis() - s_last_refresh_ms) / 1000) + "s ago";
    } else {
        s += "\nLast refresh: never";
    }
    lv_label_set_text(s_gstatus_label, s.c_str());
}

// Periodic refresh of whichever data screen is in front, plus a status tick.
static void auto_refresh_cb(lv_timer_t *t) {
    (void)t;
    update_global_status();
    if (!g_ledfx.is_connected()) return;
    uint32_t idx = lv_tabview_get_tab_act(s_tabview);
    if (idx == 0) rebuild_scene_grid();
    else if (idx == 1) rebuild_virt_list();
}

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
    lv_obj_t *ms = lv_switch_create(mrow);
    lv_obj_add_event_cb(ms, mirror_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *frow = lv_obj_create(s_tab_global);
    lv_obj_set_size(frow, LV_PCT(100), 60);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *fl = lv_label_create(frow);
    lv_label_set_text(fl, "Flip");
    lv_obj_t *fs = lv_switch_create(frow);
    lv_obj_add_event_cb(fs, flip_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
    lv_dropdown_set_options(dd, "Rainbow\nSunset\nOcean\nForest\nFire\nFrost");
    lv_obj_add_event_cb(dd, gradient_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Connection / status row
    s_gstatus_label = lv_label_create(s_tab_global);
    lv_obj_set_width(s_gstatus_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_gstatus_label, lv_color_hex(0xaaaaaa), 0);
    update_global_status();
}

// ---- Tab switching ---------------------------------------------------------
static void tab_changed_cb(lv_event_t *e) {
    lv_obj_t *btns = lv_event_get_target(e);
    uint32_t idx = lv_tabview_get_tab_act(btns);
    if (idx == 0) rebuild_scene_grid();
    if (idx == 1) rebuild_virt_list();
    if (idx == 2) update_global_status();
}

// ---- Init ------------------------------------------------------------------
void ui_init(void) {
    s_root = lv_obj_create(NULL);
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

    // Grids/lists
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {80, 80, 80, 80, 80, 80, LV_GRID_TEMPLATE_LAST};
    s_scene_grid = lv_obj_create(s_tab_scenes);
    lv_obj_set_size(s_scene_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(s_scene_grid, col_dsc, row_dsc);
    lv_obj_set_layout(s_scene_grid, LV_LAYOUT_GRID);

    s_virt_list = lv_obj_create(s_tab_virtuals);
    lv_obj_set_size(s_virt_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_virt_list, LV_FLEX_FLOW_COLUMN);

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
    lv_obj_t *psw = lv_switch_create(pause_wrap);
    lv_obj_add_event_cb(psw, pause_all_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_global_screen();
    rebuild_scene_grid();

    // Keep the front data screen (and the status row) in sync with LedFx.
    lv_timer_create(auto_refresh_cb, AUTO_REFRESH_MS, NULL);
}

void ui_show_status(const char *msg, bool is_error) {
    if (!s_status_label) return;
    lv_label_set_text(s_status_label, msg);
    lv_obj_set_style_text_color(s_status_label,
                                is_error ? lv_color_hex(0xff4444) : lv_color_hex(0xcccccc), 0);
}

void ui_refresh_scenes(void)   { rebuild_scene_grid(); }
void ui_refresh_virtuals(void) { rebuild_virt_list(); }
