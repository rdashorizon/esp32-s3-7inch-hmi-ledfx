// ---------------------------------------------------------------------------
// ui_common.cpp — implementation of the shared UI widgets. See ui_common.h.
// ---------------------------------------------------------------------------
#include "ui_common.h"
#include "ui_theme.h"   // ui_theme_*() palette accessors

// How long a transient banner stays up before dismissing itself.
static const uint32_t BANNER_TIMEOUT_MS = 3000;

void ui_obj_show(lv_obj_t *o, bool show) {
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- UiBanner --------------------------------------------------------------
void UiBanner::build(lv_obj_t *parent, lv_coord_t width) {
    _obj = lv_label_create(parent);
    lv_obj_set_width(_obj, width);
    lv_obj_set_style_bg_opa(_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_obj, 8, 0);
    lv_obj_set_style_radius(_obj, 4, 0);
    lv_label_set_text(_obj, "");
    apply_theme();
    lv_obj_add_flag(_obj, LV_OBJ_FLAG_HIDDEN);
}

void UiBanner::disarm() {
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

void UiBanner::hide_cb(lv_timer_t *t) {
    UiBanner *self = static_cast<UiBanner *>(t->user_data);
    if (!self) return;
    // LVGL deletes a repeat_count==1 timer itself once this returns, so drop
    // our handle first — deleting it here would be a double free.
    self->_timer = nullptr;
    if (self->_obj) lv_obj_add_flag(self->_obj, LV_OBJ_FLAG_HIDDEN);
}

void UiBanner::show(const char *msg) {
    if (!_obj) return;
    lv_label_set_text(_obj, msg);
    lv_obj_clear_flag(_obj, LV_OBJ_FLAG_HIDDEN);
    disarm();   // restart the countdown from this message
    _timer = lv_timer_create(hide_cb, BANNER_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(_timer, 1);
}

void UiBanner::hide() {
    disarm();
    if (_obj) lv_obj_add_flag(_obj, LV_OBJ_FLAG_HIDDEN);
}

void UiBanner::apply_theme() {
    if (!_obj) return;
    lv_obj_set_style_text_color(_obj, lv_color_hex(ui_theme_text_warn()), 0);
    lv_obj_set_style_bg_color(_obj, lv_color_hex(ui_theme_panel_alt()), 0);
}

// ---- UiColorPicker ---------------------------------------------------------
UiColorPicker *UiColorPicker::from(lv_event_t *e) {
    return static_cast<UiColorPicker *>(lv_obj_get_user_data(lv_event_get_target(e)));
}

void UiColorPicker::wheel_cb(lv_event_t *e) {
    UiColorPicker *self = from(e);
    if (!self) return;
    // Go through lv_color_to32() rather than reading c.ch.* directly. At
    // LV_COLOR_DEPTH 16 those are 5/6/5-bit bitfields, so the old
    // `r = c.ch.red` read a 0..31 value into a channel the rest of the code
    // treats as 0..255 — every wheel pick collapsed to near-black and yanked
    // the RGB sliders down with it. lv_color_to32() does the 5→8 bit
    // expansion and gives real 0..255 channels.
    lv_color32_t c32;
    c32.full = lv_color_to32(lv_colorwheel_get_rgb(lv_event_get_target(e)));
    self->_c = { c32.ch.red, c32.ch.green, c32.ch.blue };
    self->sync_sliders();     // the wheel drives the sliders, not the reverse
    self->refresh_preview();
    if (self->_on_change) self->_on_change(self->_user);
}

void UiColorPicker::slider_r_cb(lv_event_t *e) {
    UiColorPicker *self = from(e);
    if (self) self->on_slider(0, lv_slider_get_value(lv_event_get_target(e)));
}
void UiColorPicker::slider_g_cb(lv_event_t *e) {
    UiColorPicker *self = from(e);
    if (self) self->on_slider(1, lv_slider_get_value(lv_event_get_target(e)));
}
void UiColorPicker::slider_b_cb(lv_event_t *e) {
    UiColorPicker *self = from(e);
    if (self) self->on_slider(2, lv_slider_get_value(lv_event_get_target(e)));
}

void UiColorPicker::on_slider(int channel, int value) {
    uint8_t v = (uint8_t)value;
    if (channel == 0)      _c.r = v;
    else if (channel == 1) _c.g = v;
    else                   _c.b = v;
    // Deliberately not echoing back into the wheel: lv_colorwheel_set_rgb()
    // during a slider drag re-derives HSV and makes the wheel knob jump.
    refresh_preview();
    if (_on_change) _on_change(_user);
}

void UiColorPicker::sync_sliders() {
    const uint8_t ch[3] = { _c.r, _c.g, _c.b };
    for (int i = 0; i < 3; i++) {
        if (_slider[i]) lv_slider_set_value(_slider[i], ch[i], LV_ANIM_OFF);
    }
}

void UiColorPicker::refresh_preview() {
    if (_swatch) {
        lv_obj_set_style_bg_color(_swatch, lv_color_make(_c.r, _c.g, _c.b), 0);
    }
    static const char *names[3] = {"R", "G", "B"};
    const uint8_t ch[3] = { _c.r, _c.g, _c.b };
    for (int i = 0; i < 3; i++) {
        if (_label[i]) lv_label_set_text_fmt(_label[i], "%s %3d", names[i], ch[i]);
    }
}

void UiColorPicker::reset() {
    _wheel  = nullptr;
    _swatch = nullptr;
    for (int i = 0; i < 3; i++) { _slider[i] = nullptr; _label[i] = nullptr; }
    _on_change = nullptr;
    _user      = nullptr;
}

void UiColorPicker::set_rgb(LedColor c) {
    _c = c;
    if (_wheel) lv_colorwheel_set_rgb(_wheel, lv_color_make(_c.r, _c.g, _c.b));
    sync_sliders();
    refresh_preview();
}

void UiColorPicker::apply_theme() {
    if (_swatch) {
        lv_obj_set_style_border_color(_swatch, lv_color_hex(ui_theme_border()), 0);
    }
}

void UiColorPicker::build(lv_obj_t *parent, const Metrics &m,
                          ChangeCb on_change, void *user) {
    _on_change = on_change;
    _user      = user;

    // Colorwheel. knob_recolor=true tints the knob with the current color,
    // which doubles as a small inline preview.
    _wheel = lv_colorwheel_create(parent, true);
    lv_obj_set_size(_wheel, m.wheel_px, m.wheel_px);
    lv_obj_set_user_data(_wheel, this);
    lv_obj_add_event_cb(_wheel, wheel_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // R / G / B slider rows.
    static const char *names[3] = {"R", "G", "B"};
    const lv_event_cb_t cbs[3] = { slider_r_cb, slider_g_cb, slider_b_cb };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), m.row_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        _label[i] = lv_label_create(row);
        lv_label_set_text(_label[i], names[i]);   // replaced by refresh_preview()

        _slider[i] = lv_slider_create(row);
        lv_obj_set_width(_slider[i], m.slider_w);
        lv_slider_set_range(_slider[i], 0, 255);
        lv_obj_set_user_data(_slider[i], this);
        lv_obj_add_event_cb(_slider[i], cbs[i], LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Live-preview swatch.
    _swatch = lv_obj_create(parent);
    lv_obj_set_size(_swatch, LV_PCT(100), m.swatch_h);
    lv_obj_set_style_border_width(_swatch, 1, 0);
    lv_obj_set_style_radius(_swatch, 4, 0);
    apply_theme();

    // Push whatever the owner seeded into _c (via set_rgb before build, or the
    // default) onto the freshly built widgets.
    set_rgb(_c);
}
