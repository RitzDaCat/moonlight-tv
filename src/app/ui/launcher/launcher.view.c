#include "launcher.controller.h"
#include "apps.controller.h"
#include "app.h"
#include "ui/root.h"
#include "ui/tv_style.h"
#include "lvgl/ext/lv_child_group.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "util/i18n.h"

static void detail_group_add(lv_event_t *event);

static lv_obj_t *nav_button(lv_obj_t *parent, const char *text) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(button, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, LV_DPX(4), 0);
    lv_obj_set_style_pad_hor(button, LV_DPX(9), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(TV_TEXT), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_side(button, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(button, LV_DPX(1), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, lv_color_hex(TV_TEXT), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(button, lv_color_hex(TV_TEXT), LV_STATE_CHECKED);
    tv_focus_style(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t *launcher_win_create(lv_fragment_t *self, lv_obj_t *parent) {
    launcher_fragment_t *controller = (launcher_fragment_t *) self;
    controller->nav_group = lv_group_create();
    controller->detail_group = lv_group_create();
    controller->host_group = lv_group_create();

    lv_obj_t *win = lv_obj_create(parent);
    lv_obj_remove_style_all(win);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    tv_surface(win, TV_BACKGROUND);
    lv_obj_set_size(win, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *nav = controller->nav = lv_obj_create(win);
    lv_obj_remove_style_all(nav);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(nav, LV_PCT(100), LV_DPX(52));
    lv_obj_set_style_pad_hor(nav, LV_DPX(28), 0);
    lv_obj_set_style_pad_ver(nav, LV_DPX(10), 0);
    lv_obj_set_style_pad_column(nav, LV_DPX(7), 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(nav, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->nav_group);

    lv_obj_t *logo = lv_img_create(nav);
    lv_img_set_src(logo, ui_logo_src());
    lv_obj_t *brand = lv_label_create(nav);
    lv_label_set_text_static(brand, "Moonlight");
    lv_obj_set_style_text_font(brand, lv_theme_get_font_large(brand), 0);
    lv_obj_set_style_pad_right(brand, LV_DPX(20), 0);

    controller->home_btn = nav_button(nav, locstr("Home"));
    lv_obj_add_state(controller->home_btn, LV_STATE_CHECKED);
    controller->computers_btn = nav_button(nav, locstr("Computers"));
#if FEATURE_SPLIT_SCREEN
    controller->split_btn = nav_button(nav, locstr("Split screen"));
#endif
    controller->pref_btn = nav_button(nav, locstr("Settings"));
    controller->help_btn = nav_button(nav, locstr("Help"));
    lv_obj_t *spacer = lv_obj_create(nav);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    controller->host_btn = nav_button(nav, locstr("Choose computer"));
    controller->host_label = lv_obj_get_child(controller->host_btn, 0);
    lv_obj_set_style_text_font(controller->host_label, lv_theme_get_font_small(nav), 0);
    lv_label_set_long_mode(controller->host_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(controller->host_btn, LV_PCT(23), 0);

    lv_obj_t *detail = controller->detail = lv_obj_create(win);
    lv_obj_remove_style_all(detail);
    lv_obj_clear_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_obj_set_height(detail, 1);
    lv_obj_set_flex_grow(detail, 1);
    lv_obj_add_event_cb(detail, detail_group_add, LV_EVENT_CHILD_CREATED, controller);

    lv_obj_t *empty = lv_label_create(detail);
    lv_label_set_text_static(empty, locstr("Choose a computer to see your games and apps."));
    lv_obj_set_style_text_color(empty, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_width(empty, LV_PCT(80));
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(empty);

    lv_obj_t *footer = lv_obj_create(win);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_DPX(44));
    lv_obj_set_style_pad_hor(footer, LV_DPX(30), 0);
    lv_obj_set_style_pad_top(footer, LV_DPX(12), 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x293546), 0);
    controller->footer_hint = lv_label_create(footer);
    lv_label_set_text_static(controller->footer_hint,
            locstr("D-pad / stick  Move     A  Select     B  Back     LB / RB  Sections     X  Options"));
    lv_obj_set_style_text_font(controller->footer_hint, lv_theme_get_font_small(footer), 0);
    lv_obj_set_style_text_color(controller->footer_hint, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_width(controller->footer_hint, LV_PCT(100));
    lv_label_set_long_mode(controller->footer_hint, LV_LABEL_LONG_WRAP);

    /* A separate group keeps hidden computer rows out of the top navigation. */
    lv_obj_t *panel = controller->computer_panel = lv_obj_create(win);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, LV_DPX(28), LV_DPX(54));
    lv_obj_set_size(panel, LV_PCT(46), LV_PCT(76));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, LV_DPX(18), 0);
    lv_obj_set_style_pad_row(panel, LV_DPX(10), 0);
    lv_obj_set_style_radius(panel, LV_DPX(8), 0);
    tv_surface(panel, TV_SURFACE);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x34445b), 0);
    lv_obj_add_event_cb(panel, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->host_group);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text_static(title, locstr("Computers"));
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(panel), 0);
    lv_obj_t *pclist = controller->pclist = lv_list_create(panel);
    lv_obj_add_flag(pclist, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(pclist, LV_PCT(100));
    lv_obj_set_height(pclist, 1);
    lv_obj_set_flex_grow(pclist, 1);
    lv_obj_set_style_pad_all(pclist, LV_DPX(4), 0);
    lv_obj_set_style_radius(pclist, 0, 0);
    lv_obj_set_style_border_width(pclist, 0, 0);
    lv_obj_set_style_bg_opa(pclist, LV_OPA_TRANSP, 0);
    controller->add_btn = lv_list_add_btn(panel, MAT_SYMBOL_ADD_TO_QUEUE, locstr("Add computer"));
    controller->quit_btn = lv_list_add_btn(panel, MAT_SYMBOL_CLOSE, locstr("Quit Moonlight"));
    lv_obj_t *buttons[] = {controller->add_btn, controller->quit_btn};
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_btn_set_icon_font(buttons[i], lv_theme_moonlight_get_iconfont_normal(panel));
        lv_obj_set_style_bg_opa(buttons[i], LV_OPA_TRANSP, 0);
        tv_focus_style(buttons[i]);
    }
    return win;
}

static void detail_group_add(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *child = lv_event_get_param(event);
    if (!child || !lv_obj_is_group_def(child)) return;
    if (lv_obj_get_group(child)) lv_group_remove_obj(child);
    lv_group_add_obj(controller->detail_group, child);
}
