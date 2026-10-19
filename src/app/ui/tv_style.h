#pragma once

#include "lvgl.h"

/* Selection is quiet; keyboard/controller focus is bright. */
#define TV_BACKGROUND 0x0b111b
#define TV_SURFACE 0x182130
#define TV_TEXT 0xf4f7fc
#define TV_MUTED 0xaebfd5
#define TV_ACCENT 0x62b5ff
/* Computer status dot. */
#define TV_ONLINE 0x3ddc84
#define TV_WARNING 0xffb547
#define TV_ERROR 0xff6b6b

/* Screen edge inset shared by the header, hero, shelf and hints. */
#define TV_MARGIN LV_DPX(48)

#define UI_KEY_SECTION_PREV 0x110001u
#define UI_KEY_SECTION_NEXT 0x110002u

static inline void tv_focus_style(lv_obj_t *obj) {
    lv_obj_set_style_outline_color(obj, lv_color_hex(TV_TEXT), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(obj, LV_DPX(3), LV_STATE_FOCUS_KEY);
}

static inline void tv_surface(lv_obj_t *obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

/* Frosted pill: translucent at rest, solid white with dark text under key focus or press.
 * Theme styles are dropped, so focus changes are instant and start no transitions. */
static inline void tv_pill_style(lv_obj_t *obj, lv_opa_t rest_opa) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_bg_opa(obj, rest_opa, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(obj, lv_color_hex(TV_BACKGROUND), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj, lv_color_hex(TV_BACKGROUND), LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
}
