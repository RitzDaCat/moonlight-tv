#include "launcher.controller.h"
#include "apps.controller.h"
#include "app.h"
#include "app_session.h"
#include "ui/root.h"
#include "ui/tv_style.h"
#include "lvgl/ext/lv_child_group.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "util/i18n.h"

/* Gap between a tab and the accent bar marking the current section. */
#define TAB_BAR_GAP LV_DPX(3)
#define TAB_BAR_HEIGHT LV_DPX(2)
#define HINT_GLYPH_SIZE LV_DPX(19)

typedef enum hint_shape_t {
    HINT_SHAPE_CROSS = 1,
    HINT_SHAPE_CIRCLE,
    HINT_SHAPE_SQUARE,
} hint_shape_t;

static void detail_group_add(lv_event_t *event);

static void tab_ext_draw_size(lv_event_t *event);

static void tab_draw_current(lv_event_t *event);

static void hint_draw_shape(lv_event_t *event);

static lv_obj_t *plain_obj(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *small_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(parent), 0);
    lv_label_set_text_static(label, text);
    return label;
}

static lv_obj_t *nav_button(lv_obj_t *parent, const char *text) {
    lv_obj_t *button = lv_btn_create(parent);
    tv_pill_style(button, LV_OPA_TRANSP);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(button, LV_SIZE_CONTENT, LV_DPX(22));
    lv_obj_set_style_pad_hor(button, LV_DPX(9), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(TV_TEXT), LV_STATE_CHECKED);
    lv_obj_add_event_cb(button, tab_ext_draw_size, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_add_event_cb(button, tab_draw_current, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_refresh_ext_draw_size(button);
    lv_obj_center(small_label(button, text));
    return button;
}

/* A controller button: letter or PlayStation shape in a light circle; wider labels get a chip. */
static void hint_glyph(lv_obj_t *parent, const char *text, hint_shape_t shape) {
    lv_obj_t *glyph = plain_obj(parent);
    bool chip = text && text[1];
    lv_obj_set_size(glyph, chip ? LV_SIZE_CONTENT : HINT_GLYPH_SIZE, HINT_GLYPH_SIZE);
    lv_obj_set_style_pad_hor(glyph, LV_DPX(5), 0);
    lv_obj_set_style_radius(glyph, chip ? LV_DPX(5) : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glyph, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_bg_opa(glyph, LV_OPA_90, 0);
    lv_obj_set_style_text_color(glyph, lv_color_hex(TV_BACKGROUND), 0);
    if (text) {
        lv_obj_center(small_label(glyph, text));
    } else {
        lv_obj_add_event_cb(glyph, hint_draw_shape, LV_EVENT_DRAW_MAIN, (void *) (uintptr_t) shape);
    }
}

static lv_obj_t *flex_row(lv_obj_t *parent, lv_coord_t gap) {
    lv_obj_t *row = plain_obj(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    return row;
}

static lv_obj_t *hint_caption(lv_obj_t *hint, const char *text) {
    lv_obj_t *label = small_label(hint, text);
    lv_obj_set_style_text_color(label, lv_color_hex(TV_MUTED), 0);
    return label;
}

static lv_obj_t *hint_row(lv_obj_t *parent, bool playstation, lv_obj_t **select_caption) {
    lv_obj_t *row = flex_row(parent, LV_DPX(20));
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, TV_MARGIN, -LV_DPX(25));

    lv_obj_t *item = flex_row(row, LV_DPX(6));
    hint_glyph(item, playstation ? NULL : "A", HINT_SHAPE_CROSS);
    *select_caption = hint_caption(item, locstr("Play"));
    item = flex_row(row, LV_DPX(6));
    hint_glyph(item, playstation ? NULL : "B", HINT_SHAPE_CIRCLE);
    hint_caption(item, locstr("Back"));
    item = flex_row(row, LV_DPX(6));
    hint_glyph(item, playstation ? NULL : "X", HINT_SHAPE_SQUARE);
    hint_caption(item, locstr("Options"));
    item = flex_row(row, LV_DPX(6));
    hint_glyph(item, playstation ? "L1" : "LB", 0);
    hint_glyph(item, playstation ? "R1" : "RB", 0);
    hint_caption(item, locstr("Sections"));
    return row;
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

    /* Full screen, under the header and hints: the featured art runs edge to edge. */
    lv_obj_t *detail = controller->detail = plain_obj(win);
    lv_obj_add_flag(detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(detail, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(detail, detail_group_add, LV_EVENT_CHILD_CREATED, controller);

    lv_obj_t *empty = lv_label_create(detail);
    lv_label_set_text_static(empty, locstr("Choose a computer to see your games and apps."));
    lv_obj_set_style_text_color(empty, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_width(empty, LV_PCT(80));
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(empty);

    lv_obj_t *nav = controller->nav = plain_obj(win);
    lv_obj_set_pos(nav, 0, LV_DPX(20));
    lv_obj_set_size(nav, LV_PCT(100), LV_DPX(34));
    lv_obj_set_style_pad_hor(nav, TV_MARGIN, 0);
    lv_obj_set_style_pad_column(nav, LV_DPX(6), 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(nav, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->nav_group);

    lv_obj_t *logo = lv_img_create(nav);
    lv_img_set_src(logo, ui_logo_src());
    lv_obj_set_style_pad_right(logo, LV_DPX(1), 0);
    lv_obj_t *brand = lv_label_create(nav);
    lv_label_set_text_static(brand, "Moonlight");
    lv_obj_set_style_text_font(brand, lv_theme_get_font_large(brand), 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_pad_right(brand, LV_DPX(27), 0);

    controller->home_btn = nav_button(nav, locstr("Home"));
    lv_obj_add_state(controller->home_btn, LV_STATE_CHECKED);
    controller->computers_btn = nav_button(nav, locstr("Computers"));
#if FEATURE_SPLIT_SCREEN
    controller->split_btn = app_session_split_supported(controller->global) ?
                            nav_button(nav, locstr("Split screen")) : NULL;
#endif
    controller->pref_btn = nav_button(nav, locstr("Settings"));
    controller->help_btn = nav_button(nav, locstr("Help"));
    lv_obj_t *spacer = plain_obj(nav);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *host = controller->host_btn = lv_btn_create(nav);
    tv_pill_style(host, LV_OPA_TRANSP);
    lv_obj_add_flag(host, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(host, LV_SIZE_CONTENT, LV_DPX(22));
    lv_obj_set_style_pad_hor(host, LV_DPX(9), 0);
    lv_obj_set_style_pad_column(host, LV_DPX(6), 0);
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *dot = controller->host_dot = plain_obj(host);
    lv_obj_set_size(dot, LV_DPX(6), LV_DPX(6));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(TV_MUTED), 0);
    controller->host_label = small_label(host, locstr("Choose computer"));
    lv_label_set_long_mode(controller->host_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(controller->host_label, LV_DPX(180), 0);

    controller->hints[0] = hint_row(win, false, &controller->hint_select[0]);
    controller->hints[1] = hint_row(win, true, &controller->hint_select[1]);
    lv_obj_add_flag(controller->hints[1], LV_OBJ_FLAG_HIDDEN);

    /* A separate group keeps hidden computer rows out of the top navigation. */
    lv_obj_t *panel = controller->computer_panel = lv_obj_create(win);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, TV_MARGIN, LV_DPX(60));
    /* Down to just above the hints, so it covers the shelf captions too. */
    lv_obj_set_size(panel, LV_PCT(46), LV_PCT(79));
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

static void tab_ext_draw_size(lv_event_t *event) {
    lv_coord_t *size = lv_event_get_param(event);
    *size = LV_MAX(*size, TAB_BAR_GAP + TAB_BAR_HEIGHT);
}

static void tab_draw_current(lv_event_t *event) {
    lv_obj_t *tab = lv_event_get_target(event);
    if (!lv_obj_has_state(tab, LV_STATE_CHECKED)) return;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = LV_RADIUS_CIRCLE;
    bar.bg_color = lv_color_hex(TV_ACCENT);
    lv_coord_t width = LV_DPX(14);
    lv_area_t area;
    area.x1 = tab->coords.x1 + (lv_area_get_width(&tab->coords) - width) / 2;
    area.x2 = area.x1 + width - 1;
    area.y1 = tab->coords.y2 + TAB_BAR_GAP;
    area.y2 = area.y1 + TAB_BAR_HEIGHT - 1;
    lv_draw_rect(lv_event_get_draw_ctx(event), &bar, &area);
}

static void hint_draw_shape(lv_event_t *event) {
    lv_obj_t *glyph = lv_event_get_target(event);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_color_t color = lv_color_hex(TV_BACKGROUND);
    lv_coord_t stroke = LV_MAX(LV_DPX(3) / 2, 2), half = HINT_GLYPH_SIZE * 2 / 9;
    lv_coord_t cx = glyph->coords.x1 + lv_area_get_width(&glyph->coords) / 2;
    lv_coord_t cy = glyph->coords.y1 + lv_area_get_height(&glyph->coords) / 2;
    hint_shape_t shape = (hint_shape_t) (uintptr_t) lv_event_get_user_data(event);
    if (shape == HINT_SHAPE_CROSS) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.width = stroke;
        line.round_start = line.round_end = 1;
        lv_point_t a = {cx - half, cy - half}, b = {cx + half, cy + half};
        lv_draw_line(ctx, &line, &a, &b);
        a.y = cy + half;
        b.y = cy - half;
        lv_draw_line(ctx, &line, &a, &b);
        return;
    }
    if (shape == HINT_SHAPE_SQUARE) half = half * 7 / 8;
    lv_draw_rect_dsc_t outline;
    lv_draw_rect_dsc_init(&outline);
    outline.bg_opa = LV_OPA_TRANSP;
    outline.border_color = color;
    outline.border_width = stroke;
    outline.radius = shape == HINT_SHAPE_CIRCLE ? LV_RADIUS_CIRCLE : 1;
    lv_area_t area = {cx - half, cy - half, cx + half, cy + half};
    lv_draw_rect(ctx, &outline, &area);
}
