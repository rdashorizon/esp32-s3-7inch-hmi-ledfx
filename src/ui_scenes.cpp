// ---------------------------------------------------------------------------
// ui_scenes.cpp — Scenes tab implementation
//
// Owned by the LVGL thread (core 1). Do not call into the network client;
// submit worker requests and let the result pump repaint.
// ---------------------------------------------------------------------------
#include "ui_scenes.h"
#include "ui.h"        // ui_show_status, ui_submit (for the overlay), ui_global_mark_refreshed
#include "worker.h"    // worker_submit, REQ_*
#include "ui_global.h" // ui_global_mark_refreshed
#include "ui_theme.h"  // ui_theme_*() palette accessors (Tier 1.3)
#include <Arduino.h>   // millis

// ---- Module state (LVGL-thread-only) --------------------------------------
static lv_obj_t *s_scene_grid = nullptr;
static lv_obj_t *s_scene_spinner = nullptr;
static lv_obj_t *s_scene_error   = nullptr;

static SceneInfo *s_scenes = nullptr;  // owned by this module; delete[] on next fetch
static int s_scene_count = 0;

// 4-column grid; the row tracks are sized per render so any scene count
// lays out and the grid scrolls when it's taller than the screen.
static lv_coord_t s_scene_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                       LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static lv_coord_t s_scene_row_dsc[10] = {LV_GRID_TEMPLATE_LAST};  // filled per render

// ---- Helpers ---------------------------------------------------------------
static void obj_show(lv_obj_t *o, bool show) {
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- Event handlers --------------------------------------------------------
static void scene_btn_clicked(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    ui_submit(req_id(REQ_ACTIVATE_SCENE, s_scenes[idx].id));
    ui_show_status_fmt(false, "Activating: %s", s_scenes[idx].name.c_str());
}

static void scene_btn_long(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_scene_count) return;
    ui_submit(req_id(REQ_DEACTIVATE_SCENE, s_scenes[idx].id));
    ui_show_status_fmt(false, "Deactivating: %s", s_scenes[idx].name.c_str());
}

// ---- Paint -----------------------------------------------------------------
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
            lv_obj_set_style_bg_color(btn, lv_color_hex(ui_theme_accent_rgb565()), 0);
            // Active border: a brighter variant of the accent so the
            // highlight reads as "thicker" than the surrounding row. In
            // Dark mode the border is the same accent color (the button is
            // already saturated); in Light mode the border stays the same
            // color so it reads against the light background.
            lv_obj_set_style_border_color(btn, lv_color_hex(ui_theme_accent_rgb565()), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
        }
    }
    ui_show_status(s_scene_count ? "Scenes refreshed" : "No scenes found");
    ui_global_mark_refreshed();
}

// ---- Public API ------------------------------------------------------------
void ui_scenes_build(lv_obj_t *parent) {
    s_scene_grid = lv_obj_create(parent);
    lv_obj_set_size(s_scene_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(s_scene_grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(s_scene_grid, s_scene_col_dsc, s_scene_row_dsc);

    s_scene_spinner = lv_spinner_create(parent, 1000, 60);
    lv_obj_set_size(s_scene_spinner, 56, 56);
    lv_obj_center(s_scene_spinner);
    lv_obj_add_flag(s_scene_spinner, LV_OBJ_FLAG_HIDDEN);
    // Spinner arc uses the accent color so it reads as "the app is doing
    // something in your chosen theme's color".
    lv_obj_set_style_arc_color(s_scene_spinner,
        lv_color_hex(ui_theme_accent_rgb565()), LV_PART_INDICATOR);
    s_scene_error = lv_label_create(parent);
    lv_obj_set_style_text_color(s_scene_error,
        lv_color_hex(ui_theme_text_error()), 0);
    lv_obj_center(s_scene_error);
    lv_obj_add_flag(s_scene_error, LV_OBJ_FLAG_HIDDEN);
}

void ui_scenes_request_refresh(void) {
    if (ui_submit(req_simple(REQ_FETCH_SCENES))) {
        obj_show(s_scene_spinner, true);
        obj_show(s_scene_error, false);
        ui_show_status("Refreshing scenes…");
    }
}

void ui_scenes_pump_result(int status, int count, SceneInfo *data, const char *err) {
    delete[] s_scenes;  // release the previous list
    s_scenes = data;
    s_scene_count = count;
    obj_show(s_scene_spinner, false);
    if (status == 200) {
        obj_show(s_scene_error, false);
        render_scene_grid();
    } else {
        lv_obj_clean(s_scene_grid);
        if (s_scene_error)
            lv_label_set_text(s_scene_error,
                (String(LV_SYMBOL_WARNING "  ") + (err && err[0] ? err : "Couldn't reach LedFx")).c_str());
        obj_show(s_scene_error, true);
        ui_show_status(err && err[0] ? err : "Failed to fetch scenes", true);
    }
}

// ---- Theme support (Tier 1.3) -------------------------------------------
void ui_scenes_apply_theme(void) {
    if (s_scene_spinner) {
        lv_obj_set_style_arc_color(s_scene_spinner,
            lv_color_hex(ui_theme_accent_rgb565()), LV_PART_INDICATOR);
    }
    if (s_scene_error) {
        lv_obj_set_style_text_color(s_scene_error,
            lv_color_hex(ui_theme_text_error()), 0);
    }
    // The scene grid is rebuilt by render_scene_grid() on every fetch —
    // invalidating the container is enough to make the next render pick up
    // the new accent via the build-time `ui_theme_accent_rgb565()` call.
    if (s_scene_grid) lv_obj_invalidate(s_scene_grid);
}

namespace ui_scenes {
    void apply_theme() { ui_scenes_apply_theme(); }
}
