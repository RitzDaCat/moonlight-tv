#include "appitem.view.h"
#include "res.h"

#include "draw/sdl/lv_draw_sdl_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "ui/tv_style.h"

/* Key focus zooms the poster 1.08x (LVGL zoom: 256 = 1x). */
#define APPITEM_ZOOM_FOCUSED 276
#define APPITEM_ZOOM_TIME 110
#define APPITEM_RADIUS LV_DPX(7)
#define APPITEM_RING_WIDTH LV_DPX(2)
#define APPITEM_RING_PAD LV_DPX(3)
#define APPITEM_CAPTION_GAP LV_DPX(18)
#define APPITEM_CAPTION_OPA 217

static void appitem_holder_free_cb(lv_event_t *event);

static void appitem_draw_decor(lv_event_t *e);

static void appitem_ext_draw_size(lv_event_t *e);

static void appitem_focused(lv_event_t *e);

static void appitem_defocused(lv_event_t *e);

static void appitem_zoom_to(lv_obj_t *item, uint16_t zoom);

static appitem_viewholder_t *appitem_holder_new(appitem_styles_t *styles, lv_obj_t *obj);

lv_obj_t *appitem_view(appitem_styles_t *styles, lv_coord_t width, lv_coord_t height, lv_obj_t *parent) {
    lv_obj_t *item = lv_img_create(parent);
    /* The title sits below the poster, outside its box. */
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                          LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(item, &styles->cover, 0);
    lv_obj_add_style(item, &styles->cover_loaded, LV_STATE_USER_1);
    lv_obj_add_style(item, &styles->cover_focused, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(item, appitem_draw_decor, LV_EVENT_DRAW_MAIN, styles);
    lv_obj_add_event_cb(item, appitem_ext_draw_size, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_add_event_cb(item, appitem_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(item, appitem_defocused, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_set_size(item, width, height);

    lv_obj_t *play_indicator = lv_label_create(item);
    lv_obj_clear_flag(play_indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style_all(play_indicator);
    lv_obj_add_style(play_indicator, &styles->btn, 0);
    lv_label_set_text(play_indicator, MAT_SYMBOL_PLAY_ARROW);
    lv_obj_center(play_indicator);

    lv_obj_t *title = lv_label_create(item);
    lv_obj_set_style_text_font(title, lv_theme_get_font_small(item), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_style_text_opa(title, APPITEM_CAPTION_OPA, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(TV_TEXT), LV_STATE_FOCUSED);
    lv_obj_set_style_text_opa(title, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 0, height + APPITEM_CAPTION_GAP);

    appitem_viewholder_t *holder = appitem_holder_new(styles, item);
    holder->play_indicator = play_indicator;
    holder->title = title;
    return item;
}

lv_obj_t *appitem_backdrop_view(appitem_styles_t *styles, lv_obj_t *parent) {
    lv_obj_t *backdrop = lv_img_create(parent);
    lv_obj_remove_style_all(backdrop);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_img_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_img_opa(backdrop, LV_OPA_COVER, LV_STATE_USER_1);
    appitem_holder_new(styles, backdrop);
    return backdrop;
}

lv_coord_t appitem_caption_height(lv_obj_t *ref) {
    return APPITEM_CAPTION_GAP + lv_theme_get_font_small(ref)->line_height;
}

void appitem_style_init(appitem_styles_t *style) {
    /* Covers carry their rounded corners in alpha, so no clip mask is needed. */
    lv_style_init(&style->cover);
    lv_style_set_pad_all(&style->cover, 0);
    lv_style_set_radius(&style->cover, APPITEM_RADIUS);
    lv_style_set_bg_color(&style->cover, lv_color_hex(TV_SURFACE));
    lv_style_set_bg_opa(&style->cover, LV_OPA_COVER);
    lv_style_set_img_opa(&style->cover, LV_OPA_TRANSP);
    lv_style_set_border_width(&style->cover, 0);
    lv_style_set_shadow_width(&style->cover, 0);
    lv_style_set_outline_color(&style->cover, lv_color_hex(TV_TEXT));
    lv_style_set_outline_width(&style->cover, APPITEM_RING_WIDTH);
    lv_style_set_outline_pad(&style->cover, APPITEM_RING_PAD);
    lv_style_set_outline_opa(&style->cover, LV_OPA_TRANSP);

    lv_style_init(&style->cover_loaded);
    lv_style_set_bg_opa(&style->cover_loaded, LV_OPA_TRANSP);
    lv_style_set_img_opa(&style->cover_loaded, LV_OPA_COVER);

    lv_style_init(&style->cover_focused);
    lv_style_set_outline_color(&style->cover_focused, lv_color_hex(TV_TEXT));
    lv_style_set_outline_opa(&style->cover_focused, LV_OPA_COVER);

    lv_style_init(&style->btn);
    lv_style_set_width(&style->btn, LV_DPX(40));
    lv_style_set_height(&style->btn, LV_DPX(40));
    lv_style_set_radius(&style->btn, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&style->btn, lv_color_white());
    lv_style_set_bg_opa(&style->btn, LV_OPA_COVER);
    lv_style_set_text_color(&style->btn, lv_color_black());
    lv_style_set_border_opa(&style->btn, LV_OPA_TRANSP);
    lv_style_set_text_font(&style->btn, lv_theme_moonlight_get_iconfont_large(lv_scr_act()));
    lv_style_set_shadow_width(&style->btn, LV_DPX(4));
    lv_style_set_shadow_color(&style->btn, lv_color_black());
    lv_style_set_shadow_opa(&style->btn, LV_OPA_30);
    /* Temporary solution for LVGL 8.3.0 */
    lv_style_set_text_align(&style->btn, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_ver(&style->btn, LV_DPX(10));

    style->placeholder_font = lv_theme_moonlight_get_iconfont_display(lv_scr_act());

    style->fav_indicator_src.header.w = LV_DPX(48);
    style->fav_indicator_src.header.h = LV_DPX(48);
    style->fav_indicator_src.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    style->fav_indicator_src.data_size = sizeof(lv_sdl_img_data_t);
    style->fav_indicator_src.data = (const uint8_t *) &lv_sdl_img_data_fav_indicator;

    style->defcover_src.header.w = 0;
    style->defcover_src.header.h = 0;
    style->defcover_src.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    style->defcover_src.data_size = sizeof(lv_sdl_img_data_t);
    style->defcover_src.data = (const uint8_t *) &lv_sdl_img_data_defcover;
}

void appitem_style_deinit(appitem_styles_t *style) {
    lv_style_reset(&style->cover);
    lv_style_reset(&style->cover_loaded);
    lv_style_reset(&style->cover_focused);
    lv_style_reset(&style->btn);
}

static appitem_viewholder_t *appitem_holder_new(appitem_styles_t *styles, lv_obj_t *obj) {
    appitem_viewholder_t *holder = calloc(1, sizeof(appitem_viewholder_t));
    holder->styles = styles;
    lv_obj_set_user_data(obj, holder);
    lv_obj_add_event_cb(obj, appitem_holder_free_cb, LV_EVENT_DELETE, holder);
    return holder;
}

static void appitem_holder_free_cb(lv_event_t *event) {
    appitem_viewholder_t *holder = event->user_data;
    free(holder);
}

/* Placeholder glyph until the cover loads, and the favorite badge; both follow the zoom. */
static void appitem_draw_decor(lv_event_t *e) {
    const appitem_styles_t *styles = lv_event_get_user_data(e);
    lv_obj_t *item = lv_event_get_target(e);
    const appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    bool loaded = lv_obj_has_state(item, LV_STATE_USER_1);
    if (loaded && !holder->fav) return;

    lv_point_t pivot;
    lv_img_get_pivot(item, &pivot);
    lv_area_t area;
    _lv_img_buf_get_transformed_area(&area, lv_obj_get_width(item), lv_obj_get_height(item), 0,
                                     lv_img_get_zoom(item), &pivot);
    lv_area_move(&area, item->coords.x1, item->coords.y1);
    lv_opa_t opa = lv_obj_get_style_opa_recursive(item, LV_PART_MAIN);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    if (!loaded) {
        lv_draw_label_dsc_t glyph;
        lv_draw_label_dsc_init(&glyph);
        glyph.font = styles->placeholder_font;
        glyph.color = lv_color_hex(TV_MUTED);
        glyph.opa = (lv_opa_t) (LV_OPA_40 * opa / LV_OPA_COVER);
        glyph.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t line = area;
        line.y1 = area.y1 + (lv_area_get_height(&area) - glyph.font->line_height) / 2;
        line.y2 = line.y1 + glyph.font->line_height - 1;
        lv_draw_label(ctx, &glyph, &line, MAT_SYMBOL_SPORTS_ESPORTS, NULL);
    }
    if (holder->fav) {
        lv_area_t badge = area;
        lv_area_set_width(&badge, styles->fav_indicator_src.header.w);
        lv_area_set_height(&badge, styles->fav_indicator_src.header.h);
        lv_draw_img_dsc_t dsc;
        lv_draw_img_dsc_init(&dsc);
        dsc.opa = opa;
        lv_draw_img(ctx, &dsc, &badge, &styles->fav_indicator_src);
    }
}

/* Always reserve the focused size and ring: lv_img_set_zoom under-invalidates when shrinking. */
static void appitem_ext_draw_size(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_target(e);
    lv_coord_t *size = lv_event_get_param(e);
    lv_coord_t grow = LV_MAX(lv_obj_get_width(item), lv_obj_get_height(item)) *
                      (APPITEM_ZOOM_FOCUSED - LV_IMG_ZOOM_NONE) / (2 * LV_IMG_ZOOM_NONE) + 1;
    *size = LV_MAX(*size, grow + APPITEM_RING_WIDTH + APPITEM_RING_PAD + 1);
}

static void appitem_focused(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_target(e);
    const appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    if (!lv_obj_has_state(item, LV_STATE_FOCUS_KEY) || lv_obj_has_state(holder->title, LV_STATE_FOCUSED)) return;
    appitem_zoom_to(item, APPITEM_ZOOM_FOCUSED);
    lv_obj_add_state(holder->title, LV_STATE_FOCUSED);
    lv_label_set_long_mode(holder->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

static void appitem_defocused(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_target(e);
    const appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    if (!lv_obj_has_state(holder->title, LV_STATE_FOCUSED)) return;
    appitem_zoom_to(item, LV_IMG_ZOOM_NONE);
    lv_obj_clear_state(holder->title, LV_STATE_FOCUSED);
    lv_label_set_long_mode(holder->title, LV_LABEL_LONG_DOT);
}

static void appitem_zoom_exec(void *item, int32_t zoom) {
    lv_img_set_zoom(item, (uint16_t) zoom);
    lv_obj_invalidate(item);
}

static void appitem_zoom_to(lv_obj_t *item, uint16_t zoom) {
    uint16_t current = lv_img_get_zoom(item);
    if (current == zoom) {
        lv_anim_del(item, appitem_zoom_exec);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, item);
    lv_anim_set_exec_cb(&anim, appitem_zoom_exec);
    lv_anim_set_values(&anim, current, zoom);
    lv_anim_set_time(&anim, APPITEM_ZOOM_TIME * LV_ABS(zoom - current) / (APPITEM_ZOOM_FOCUSED - LV_IMG_ZOOM_NONE));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}
