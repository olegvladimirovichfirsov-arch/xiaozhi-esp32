#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_28);

namespace ModularStyle {
constexpr uint32_t kAccent = 0xD7FF1E;
constexpr uint32_t kText = 0xF2F2F7;
constexpr uint32_t kSecondary = 0xAEAEB2;
constexpr uint32_t kSurface = 0x1C1C1E;
constexpr uint32_t kActive = 0x34420A;
constexpr int kRadius = 16;
constexpr int kListWidth = 248;
constexpr int kListHeight = 224;
constexpr int kListTop = 76;

inline void PressFeedback(lv_obj_t* obj) {
    // Retain static fonts even when Assets::Apply replaces the theme font.
    lv_obj_set_style_text_font(obj, &ui_font_20, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
                                            LV_STYLE_BG_OPA, LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t press;
    static lv_style_transition_dsc_t release;
    static bool initialized = false;
    if (!initialized) {
        lv_style_transition_dsc_init(&press, props, lv_anim_path_ease_out, 100, 0, nullptr);
        lv_style_transition_dsc_init(&release, props, lv_anim_path_overshoot, 220, 0, nullptr);
        initialized = true;
    }
    lv_obj_set_style_transition(obj, &release, 0);
    lv_obj_set_style_transition(obj, &press, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_x(obj, 256, 0);
    lv_obj_set_style_transform_scale_y(obj, 256, 0);
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(obj, 248, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(obj, 248, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, 190, LV_STATE_PRESSED);
}
inline void ScrollEasing(lv_event_t* event) {
    auto* animation = static_cast<lv_anim_t*>(lv_event_get_param(event));
    if (animation != nullptr) {
        lv_anim_set_duration(animation, 280);
        lv_anim_set_path_cb(animation, lv_anim_path_ease_out);
    }
}
inline void Fade(lv_obj_t* obj) { lv_obj_fade_in(obj, 180, 0); }
}  // namespace ModularStyle
