//
// Created by Mariotaku on 2021/08/31.
//

#include "app.h"
#include "appitem.view.h"
#include "apps.controller.h"
#include "launcher.controller.h"
#include "ui/streaming/streaming.controller.h"

#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include <errors.h>
#include <assert.h>

#include "lvgl/lv_ext_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/util/lv_app_utils.h"
#include "lv_gridview.h"

#include "util/user_event.h"
#include "util/i18n.h"
#include "pair.dialog.h"
#include "ui/common/progress_dialog.h"
#include "ui/tv_style.h"

static void featured_click(lv_event_t *event);
static void featured_options(lv_event_t *event);
static void featured_key(lv_event_t *event);
static void shelf_key(lv_event_t *event);
static void shelf_focused(lv_event_t *event);
static void set_featured(apps_fragment_t *controller, int app_id);
static lv_obj_t *hero_pill(lv_obj_t *parent);
static lv_obj_t *hero_pill_label(lv_obj_t *pill, const char *text);
static void options_draw_dots(lv_event_t *event);
static void focus_detail(apps_fragment_t *controller, lv_obj_t *target);

/* Hero pills: white at 14% with a 16% hairline, as frosted glass over the backdrop. */
#define HERO_PILL_OPA 36
#define HERO_PILL_BORDER_OPA 41

typedef void (*action_cb_t)(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj);

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void on_host_updated(const uuidstr_t *uuid, void *userdata);

static void on_host_removed(const uuidstr_t *uuid, void *userdata);

static void item_click_cb(lv_event_t *event);

static void item_longpress_cb(lv_event_t *event);

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_quit_game(apps_fragment_t *controller);

static void applist_focus_enter(lv_event_t *event);

static void applist_focus_leave(lv_event_t *event);

static void update_view_state(apps_fragment_t *controller);

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app);

static int adapter_item_count(lv_obj_t *, void *data);

static lv_obj_t *adapter_create_view(lv_obj_t *parent);

static void adapter_bind_view(lv_obj_t *, lv_obj_t *, void *data, int position);

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void apps_controller_ctor(lv_fragment_t *self, void *args);

static void apps_controller_dtor(lv_fragment_t *self);

static void appload_started(void *userdata);

static void appload_loaded(apploader_list_t *apps, void *userdata);

static void appload_errored(int code, const char *error, void *userdata);

static void quit_dialog_cb(lv_event_t *event);

static void actions_click_cb(lv_event_t *event);

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void update_grid_config(apps_fragment_t *controller);

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder);

static void context_menu_cancel_cb(lv_event_t *event);

static void context_menu_click_cb(lv_event_t *event);

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app);

static void app_detail_click_cb(lv_event_t *event);

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks);

/**
 *
 * @param old_list
 * @param new_list
 * @param num_changes number of changes. It will be assigned to -1 if the whole dataset has been changed.
 * @return Allocated array of changes. It should be freed by caller.
 */
static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list,
                                                          const apploader_list_t *new_list, int *num_changes);

static void show_progress(apps_fragment_t *fragment);

static void show_ok(apps_fragment_t *fragment);

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail);

static apps_fragment_t *current_instance = NULL;

const static lv_gridview_adapter_t apps_adapter = {
        .item_count = adapter_item_count,
        .create_view = adapter_create_view,
        .bind_view = adapter_bind_view,
};
const static pcmanager_listener_t pc_listeners = {
        .updated = on_host_updated,
        .removed = on_host_removed,
};

const lv_fragment_class_t apps_controller_class = {
        .constructor_cb = apps_controller_ctor,
        .destructor_cb = apps_controller_dtor,
        .create_obj_cb = apps_view,
        .obj_will_delete_cb = obj_will_delete,
        .obj_created_cb = on_view_created,
        .obj_deleted_cb = on_destroy_view,
        .event_cb = on_event,
        .instance_size = sizeof(apps_fragment_t),
};

static const char *action_labels_offline[] = {translatable("Wake"), translatable("Retry"), ""};
static const action_cb_t action_callbacks_offline[] = {action_cb_wol, action_cb_host_reload};
static const char *action_labels_error[] = {translatable("Retry"), ""};
static const action_cb_t action_callbacks_error[] = {action_cb_host_reload};
static const char *actions_unpaired[] = {translatable("Pair"), ""};
static const action_cb_t action_callbacks_unpaired[] = {action_cb_pair};
static const char *actions_apps_none[] = {""};

static void apps_controller_ctor(lv_fragment_t *self, void *args) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->apploader_cb.start = appload_started;
    controller->apploader_cb.data = appload_loaded;
    controller->apploader_cb.error = appload_errored;
    apps_fragment_arg_t *arg = args;
    controller->global = arg->global;
    controller->uuid = arg->host;
    controller->def_app = arg->def_app;
    controller->apploader = apploader_create(arg->global, &controller->uuid, &controller->apploader_cb, controller);

    appitem_style_init(&controller->appitem_style);
    appitem_style_init(&controller->hero_style);
}

static void apps_controller_dtor(lv_fragment_t *self) {
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    appitem_style_deinit(&fragment->appitem_style);
    appitem_style_deinit(&fragment->hero_style);
    apploader_destroy(fragment->apploader);
    if (fragment->apploader_apps != NULL) {
        apploader_list_free(fragment->apploader_apps);
    }
}

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    lv_obj_t *view = lv_obj_create(container);
    lv_obj_remove_style_all(view);
    tv_surface(view, TV_BACKGROUND);
    lv_obj_add_flag(view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(view, LV_DIR_NONE);

    lv_obj_t *hero = controller->hero = lv_obj_create(view);
    lv_obj_remove_style_all(hero);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(hero, LV_PCT(100), LV_PCT(100));

    controller->hero_image = appitem_backdrop_view(&controller->hero_style, hero);
    lv_obj_set_size(controller->hero_image, LV_PCT(100), LV_PCT(100));

    lv_obj_t *feature = lv_obj_create(hero);
    lv_obj_remove_style_all(feature);
    lv_obj_clear_flag(feature, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(feature, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_pos(feature, TV_MARGIN, LV_DPX(98));
    lv_obj_set_size(feature, LV_PCT(60), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(feature, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(feature, LV_DPX(20), 0);
    controller->hero_title = lv_label_create(feature);
    lv_obj_set_width(controller->hero_title, LV_PCT(100));
    lv_label_set_long_mode(controller->hero_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(controller->hero_title, controller->global->ui.fonts.fonts.display, 0);
    lv_label_set_text(controller->hero_title, locstr("Your games, on the big screen"));

    lv_obj_t *primary_actions = lv_obj_create(feature);
    lv_obj_remove_style_all(primary_actions);
    lv_obj_add_flag(primary_actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(primary_actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(primary_actions, LV_DPX(10), 0);
    lv_obj_set_flex_flow(primary_actions, LV_FLEX_FLOW_ROW);

    lv_obj_t *launch = controller->launch_btn = hero_pill(primary_actions);
    lv_obj_t *play_icon = lv_label_create(launch);
    lv_obj_set_style_text_font(play_icon, lv_theme_moonlight_get_iconfont_small(launch), 0);
    lv_label_set_text_static(play_icon, MAT_SYMBOL_PLAY_ARROW);
    controller->launch_label = hero_pill_label(launch, locstr("Play"));
    lv_obj_add_event_cb(launch, featured_click, LV_EVENT_CLICKED, controller);
    lv_obj_add_event_cb(launch, featured_key, LV_EVENT_KEY, controller);

    lv_obj_t *options = controller->options_btn = hero_pill(primary_actions);
    lv_obj_t *dots = lv_obj_create(options);
    lv_obj_remove_style_all(dots);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dots, LV_DPX(12), LV_DPX(6));
    lv_obj_add_event_cb(dots, options_draw_dots, LV_EVENT_DRAW_MAIN, NULL);
    hero_pill_label(options, locstr("Options"));
    lv_obj_add_event_cb(options, featured_options, LV_EVENT_CLICKED, controller);
    lv_obj_add_event_cb(options, featured_key, LV_EVENT_KEY, controller);

    controller->shelf_title = lv_label_create(view);
    lv_label_set_text_static(controller->shelf_title, locstr("Games & apps"));
    lv_obj_set_style_text_font(controller->shelf_title, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(controller->shelf_title, lv_color_hex(TV_MUTED), 0);
    lv_obj_set_pos(controller->shelf_title, TV_MARGIN, LV_DPX(244));

    /* One row of posters in view; moving down snaps the next row into the same place. */
    lv_obj_t *applist = controller->applist = lv_gridview_create(view);
    lv_obj_add_flag(applist, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(applist, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(applist, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(applist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(applist, TV_MARGIN, 0);
    lv_obj_set_style_pad_top(applist, LV_DPX(14), 0);
    lv_obj_set_style_pad_bottom(applist, LV_DPX(4), 0);
    lv_obj_set_style_pad_gap(applist, LV_DPX(22), 0);
    lv_obj_set_style_radius(applist, 0, 0);
    lv_obj_set_style_border_width(applist, 0, 0);
    lv_obj_set_style_bg_opa(applist, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(applist, 0, LV_DPX(260));
    lv_obj_set_size(applist, LV_PCT(100), LV_DPX(227));
    lv_gridview_set_adapter(applist, &apps_adapter);
    lv_obj_add_event_cb(applist, shelf_key, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(applist, shelf_focused, LV_EVENT_FOCUSED, controller);

    lv_obj_t *appload = controller->appload = lv_spinner_create(view, 1000, 60);
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    lv_group_add_obj(parent_controller->detail_group, appload);
    lv_obj_add_flag(appload, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(appload, LV_DPX(45), LV_DPX(45));
    lv_obj_center(appload);

    lv_obj_t *apperror = controller->apperror = lv_obj_create(view);
    lv_obj_add_flag(apperror, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(apperror, LV_PCT(75), LV_PCT(58));
    lv_obj_center(apperror);
    tv_surface(apperror, TV_BACKGROUND);
    lv_obj_set_flex_flow(apperror, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apperror, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(apperror, LV_DPX(16), 0);
    controller->errortitle = lv_label_create(apperror);
    lv_obj_set_width(controller->errortitle, LV_PCT(100));
    lv_obj_set_style_text_font(controller->errortitle, controller->global->ui.fonts.fonts.display, 0);
    controller->errorhint = lv_label_create(apperror);
    lv_obj_set_width(controller->errorhint, LV_PCT(100));
    lv_obj_set_style_text_color(controller->errorhint, lv_color_hex(TV_MUTED), 0);
    controller->errordetail = lv_label_create(apperror);
    lv_obj_set_width(controller->errordetail, LV_PCT(100));
    lv_obj_set_style_text_font(controller->errordetail, lv_theme_get_font_small(view), 0);
    lv_obj_set_flex_grow(controller->errordetail, 1);

    lv_obj_t *actions = controller->actions = lv_btnmatrix_create(apperror);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_height(actions, LV_DPX(44));
    lv_obj_set_style_max_width(actions, LV_PCT(100), 0);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    tv_focus_style(actions);
    lv_btnmatrix_set_btn_ctrl_all(actions, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);
    set_actions(controller, actions_apps_none, NULL);
    return view;
}

static lv_obj_t *hero_pill(lv_obj_t *parent) {
    lv_obj_t *pill = lv_btn_create(parent);
    tv_pill_style(pill, HERO_PILL_OPA);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(TV_TEXT), 0);
    lv_obj_set_style_border_opa(pill, HERO_PILL_BORDER_OPA, 0);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_DPX(32));
    lv_obj_set_style_pad_left(pill, LV_DPX(14), 0);
    lv_obj_set_style_pad_right(pill, LV_DPX(17), 0);
    lv_obj_set_style_pad_column(pill, LV_DPX(7), 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);
    return pill;
}

static lv_obj_t *hero_pill_label(lv_obj_t *pill, const char *text) {
    lv_obj_t *label = lv_label_create(pill);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(pill), 0);
    lv_label_set_text_static(label, text);
    return label;
}

/* The icon font has no "more" glyph: three dots in the pill's text color. */
static void options_draw_dots(lv_event_t *event) {
    lv_obj_t *dots = lv_event_get_target(event);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = lv_obj_get_style_text_color(dots, LV_PART_MAIN);
    lv_coord_t step = lv_obj_get_width(dots) / 3;
    lv_coord_t r = step * 5 / 16;
    lv_coord_t cy = (dots->coords.y1 + dots->coords.y2) / 2;
    for (int i = 0; i < 3; i++) {
        lv_coord_t cx = dots->coords.x1 + step / 2 + i * step;
        lv_area_t dot = {cx - r, cy - r, cx + r, cy + r};
        lv_draw_rect(lv_event_get_draw_ctx(event), &dsc, &dot);
    }
}

/* Focus inside the apps page; the ring only shows while the page has the input. */
static void focus_detail(apps_fragment_t *controller, lv_obj_t *target) {
    launcher_fragment_t *parent = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    lv_group_focus_obj(target);
    if (app_input_get_group(&controller->global->ui.input) == parent->detail_group) {
        lv_obj_add_state(target, LV_STATE_FOCUS_KEY);
    } else {
        lv_obj_clear_state(target, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->coverloader = coverloader_new(controller->global);
    pcmanager_register_listener(pcmanager, &pc_listeners, controller);
    lv_obj_t *applist = controller->applist;
    lv_obj_add_event_cb(applist, item_click_cb, LV_EVENT_SHORT_CLICKED, controller);
    lv_obj_add_event_cb(applist, item_longpress_cb, LV_EVENT_LONG_PRESSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_enter, LV_EVENT_FOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_DEFOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_LEAVE, controller);
    lv_obj_add_event_cb(controller->actions, actions_click_cb, LV_EVENT_VALUE_CHANGED, controller);

    update_grid_config(controller);
    lv_obj_set_user_data(controller->applist, controller);

    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state != NULL && state->code != SERVER_STATE_QUERYING) {
        pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
        if (state->code == SERVER_STATE_AVAILABLE) {
            apploader_load(controller->apploader);
        }
    }
    current_instance = controller;
    update_view_state(controller);
}

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    apploader_cancel(fragment->apploader);
    fragment->def_app = 0;
}

static void update_grid_config(apps_fragment_t *controller) {
    lv_obj_t *applist = controller->applist;
    lv_obj_update_layout(applist);
    lv_coord_t gap = lv_obj_get_style_pad_column(applist, 0);
    lv_coord_t content_width = lv_obj_get_width(applist) - lv_obj_get_style_pad_left(applist, 0) -
                               lv_obj_get_style_pad_right(applist, 0);
    /* 3:4 posters about 240 px wide at 1080p, as many as fit. */
    int col_count = LV_MAX(1, (content_width + gap) / (LV_DPX(120) + gap));
    lv_coord_t col_width = (content_width - gap * (col_count - 1)) / col_count;
    lv_coord_t col_height = col_width * 4 / 3;
    controller->col_count = col_count;
    controller->col_width = col_width;
    controller->col_height = col_height;
    lv_gridview_set_config(applist, col_count, col_height + appitem_caption_height(applist),
                           LV_GRID_ALIGN_START, LV_GRID_ALIGN_START);

    controller->appitem_style.defcover_src.header.w = col_width;
    controller->appitem_style.defcover_src.header.h = col_height;
    lv_obj_update_layout(controller->hero_image);
    controller->hero_style.defcover_src.header.w = lv_obj_get_width(controller->hero_image);
    controller->hero_style.defcover_src.header.h = lv_obj_get_height(controller->hero_image);
    int selected = controller->featured_id;
    controller->featured_id = 0;
    set_featured(controller, selected);
}

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    current_instance = NULL;
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->show_hidden_apps = false;
    pcmanager_unregister_listener(pcmanager, &pc_listeners);
    coverloader_unref(controller->coverloader);
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    switch (code) {
        case USER_SIZE_CHANGED: {
            update_grid_config(controller);
            lv_gridview_rebind(controller->applist);
            break;
        }
        case USER_SHOW_HIDDEN_APPS: {
            ui_userevent_t *event = userdata;
            if (uuidstr_t_equals_t(&controller->uuid, event->data1)) {
                controller->show_hidden_apps = true;
                apploader_load(controller->apploader);
            }
            free(event->data1);
            return true;
        }
        default:
            break;
    }
    return false;
}

static void on_host_updated(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (controller != current_instance) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    const SERVER_STATE *state = pcmanager_state(pcmanager, uuid);
    if (state == NULL) { return; }
    if (state->code == SERVER_STATE_AVAILABLE) {
        apploader_load(controller->apploader);
    }
    update_view_state(controller);
}

static void on_host_removed(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    lv_fragment_del((lv_fragment_t *) controller);
}

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
}

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) { return; }
    if (state->code & SERVER_STATE_ONLINE || result != GS_OK) { return; }
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void update_view_state(apps_fragment_t *controller) {
    if (controller != current_instance) { return; }
    if (!controller->base.managed->obj_created || controller->base.managed->destroying_obj) { return; }
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    parent_controller->detail_changing = true;
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) {
        parent_controller->detail_changing = false;
        return;
    }
    switch (state->code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING: {
            // waiting to load server info
            show_progress(controller);
            break;
        }
        case SERVER_STATE_AVAILABLE: {
            switch (apploader_state(controller->apploader)) {
                case APPLOADER_STATE_LOADING: {
                    // is loading apps
                    if (controller->apploader_apps) {
                        break;
                    }
                    show_progress(controller);
                    break;
                }
                case APPLOADER_STATE_ERROR: {
                    // apploader has error
                    show_error(controller, locstr("Failed to load apps"),
                               locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                               controller->apploader_error);

                    set_actions(controller, action_labels_error, action_callbacks_error);
                    focus_detail(controller, controller->actions);
                    break;
                }
                case APPLOADER_STATE_IDLE: {
                    // has apps
                    if (adapter_item_count(controller->applist, controller->apploader_apps) > 0) {
                        show_ok(controller);
                    } else {
                        show_error(controller, locstr("No apps yet"),
                                   locstr("Add games or apps on your computer, then choose Retry."), NULL);
                        set_actions(controller, action_labels_error, action_callbacks_error);
                    }
                    break;
                }
            }
            break;
        }
        case SERVER_STATE_NOT_PAIRED: {
            show_error(controller, locstr("Not paired"),
                       locstr("Press \"Pair\" to pair this host with your account."),
                       NULL);

            set_actions(controller, actions_unpaired, action_callbacks_unpaired);
            focus_detail(controller, controller->actions);
            break;
        }
        case SERVER_STATE_ERROR: {
            // server has error
            show_error(controller, locstr("Host error"),
                       locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                       state->error.errmsg);

            set_actions(controller, action_labels_error, action_callbacks_error);
            focus_detail(controller, controller->actions);
            break;
        }
        case SERVER_STATE_OFFLINE: {
            // server has error
            show_error(controller, locstr("Offline"),
                       locstr("Press \"Wake\" to send Wake-on-LAN packet to turn the computer on if it supports this feature, "
                              "press \"Retry\" to connect again.\n\n"
                              "Try restart your computer if these doesn't work."),
                       NULL);

            set_actions(controller, action_labels_offline, action_callbacks_offline);
            lv_btnmatrix_clear_btn_ctrl(controller->actions, 0, LV_BTNMATRIX_CTRL_DISABLED);
            focus_detail(controller, controller->actions);
            break;
        }
        default: {
            break;
        }
    }
    parent_controller->detail_changing = false;
}

static void show_progress(apps_fragment_t *fragment) {
    lv_obj_add_flag(fragment->hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->shelf_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);

    focus_detail(fragment, fragment->appload);
}

static void show_ok(apps_fragment_t *fragment) {
    lv_obj_clear_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);
    lv_obj_clear_flag(fragment->hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->shelf_title, LV_OBJ_FLAG_HIDDEN);
    set_featured(fragment, fragment->featured_id);
    launcher_fragment_t *parent = (launcher_fragment_t *) lv_fragment_get_parent(&fragment->base);
    lv_obj_t *focused = lv_group_get_focused(parent->detail_group);
    if (app_input_get_group(&fragment->global->ui.input) == parent->detail_group &&
        (focused == fragment->appload || focused == fragment->actions || !focused)) {
        apps_focus_primary(fragment);
    }
}

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail) {
    lv_obj_add_flag(fragment->hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->shelf_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(fragment->actions, LV_STATE_DISABLED);

    lv_obj_set_style_opa(fragment->errordetail, detail ? LV_OPA_100 : LV_OPA_0, 0);
    lv_label_set_text_static(fragment->errortitle, title);
    lv_label_set_text_static(fragment->errorhint, hint);
    lv_label_set_text(fragment->errordetail, detail ? detail : "");
}

static void appload_started(void *userdata) {
    apps_fragment_t *fragment = userdata;
    update_view_state(fragment);
}

static void appload_loaded(apploader_list_t *apps, void *userdata) {
    apps_fragment_t *fragment = userdata;
    if (!fragment->base.managed->obj_created || fragment->base.managed->destroying_obj) {
        return;
    }
    int num_changes = -1;
    lv_gridview_data_change_t *changes = apps_list_detect_change(fragment->apploader_apps, apps, &num_changes);
    if (num_changes != 0) {
        lv_gridview_focus(fragment->applist, -1);
    }
    lv_gridview_set_data_advanced(fragment->applist, apps, changes, num_changes);
    if (changes != NULL) {
        free(changes);
    }
    apploader_list_free(fragment->apploader_apps);
    fragment->apploader_apps = apps;
    set_featured(fragment, fragment->featured_id);
    update_view_state(fragment);

    if (fragment->def_app > 0 && !fragment->def_app_launched) {
        fragment->def_app_launched = true;
        const apploader_item_t *app = NULL;
        for (int i = 0; i < apps->count; ++i) {
            if (apps->items[i].base.id == fragment->def_app) {
                app = &apps->items[i];
                break;
            }
        }
        if (app != NULL) {
            launcher_launch_game(fragment, app);
        }
    }
}

static void appload_errored(int code, const char *error, void *userdata) {
    LV_UNUSED(code);
    apps_fragment_t *fragment = userdata;
    fragment->apploader_error = error;
    update_view_state(fragment);
}

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);

    coverloader_display(controller->coverloader, &controller->uuid, app->base.id, item,
                        controller->col_width, controller->col_height);
    lv_label_set_text(holder->title, app->base.name);

    int current_id = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (current_id == app->base.id) {
        lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(item, app->hidden ? LV_OPA_50 : LV_OPA_COVER, 0);
    holder->app_id = app->base.id;
    holder->fav = app->fav;
}

static void item_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    appitem_viewholder_t *holder = (appitem_viewholder_t *) lv_obj_get_user_data(target);
    int current_app = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (current_app != 0) {
        if (holder->app_id == current_app) {
            open_context_menu(controller, holder);
        }
        return;
    }
    const apploader_item_t *item = apploader_list_item_by_id(controller->apploader_apps, holder->app_id);
    assert(item != NULL);
    launcher_launch_game(controller, item);
}

static void item_longpress_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    lv_event_send(target, LV_EVENT_RELEASED, lv_event_get_indev(event));
    open_context_menu(controller, (appitem_viewholder_t *) lv_obj_get_user_data(target));
}

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app) {
    LV_ASSERT(app->base.id != 0);
    streaming_scene_arg_t args = {
            .global = controller->global,
            .uuid = controller->uuid,
            .app = app->base,
    };
    app_ui_t *ui = &controller->global->ui;
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
}

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_favorite_app(pcmanager, &controller->uuid, app->base.id, !app->fav);
    apploader_load(controller->apploader);
}

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_set_app_hidden(pcmanager, &controller->uuid, app->base.id, !app->hidden);
    controller->show_hidden_apps = true;
    apploader_load(controller->apploader);
}

static void launcher_quit_game(apps_fragment_t *controller) {
    controller->quit_progress = progress_dialog_create(locstr("Quitting game..."));
    pcmanager_quitapp(pcmanager, &controller->uuid, quitgame_cb, NULL);
}

static int adapter_item_count(lv_obj_t *grid, void *data) {
    if (data == NULL) { return 0; }
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    apploader_list_t *list = data;
    // LVGL can only display up to 255 rows/columns, but I don't think anyone has library that big (1275 items)
    int count = LV_MIN(list->count, 255 * controller->col_count);
    if (!controller->show_hidden_apps) {
        for (int i = 0; i < count; i++) {
            if (list->items[i].hidden) {
                count = i;
                break;
            }
        }
    }
    return count;
}

static lv_obj_t *adapter_create_view(lv_obj_t *parent) {
    apps_fragment_t *controller = lv_obj_get_user_data(parent);
    return appitem_view(&controller->appitem_style, controller->col_width, controller->col_height, parent);
}

static void adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    apploader_list_t *list = data;
    appitem_bind(controller, item_view, &list->items[position]);

    // IDE seems to be pretty confused...
    LV_UNUSED(list);
    LV_UNUSED(position);
}


static void applist_focus_enter(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    if (!controller->base.managed || controller->base.managed->destroying_obj) return;
    lv_obj_t *applist = controller->applist;
    lv_gridview_focus(applist, controller->focus_backup);
    /* Focused from code there is no active input device, so the item can't tell it got key focus. */
    if (lv_indev_get_act() || !lv_obj_has_state(applist, LV_STATE_FOCUS_KEY)) return;
    lv_obj_t *item = lv_gridview_get_item_view(applist, lv_gridview_get_focused_index(applist));
    if (item) lv_event_send(item, LV_EVENT_FOCUSED, lv_event_get_indev(event));
}

static void applist_focus_leave(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    if (!controller->base.managed || controller->base.managed->destroying_obj) return;
    /* Leaving twice (blur, then the next focus) must not lose the position. */
    int focused = lv_gridview_get_focused_index(controller->applist);
    if (focused >= 0) controller->focus_backup = focused;
    lv_gridview_focus(controller->applist, -1);
}

void apps_focus_primary(apps_fragment_t *controller) {
    lv_obj_t *target = controller->launch_btn;
    if (!lv_obj_has_flag(controller->apperror, LV_OBJ_FLAG_HIDDEN)) target = controller->actions;
    else if (!lv_obj_has_flag(controller->appload, LV_OBJ_FLAG_HIDDEN)) target = controller->appload;
    else if (lv_obj_has_state(target, LV_STATE_DISABLED)) target = controller->options_btn;
    focus_detail(controller, target);
}

bool apps_focus_plays(apps_fragment_t *controller, lv_obj_t *focused) {
    return focused == controller->launch_btn || focused == controller->applist;
}

static void set_featured(apps_fragment_t *controller, int app_id) {
    if (!controller->base.managed || controller->base.managed->destroying_obj ||
        !controller->base.managed->obj_created) return;
    const apploader_item_t *app = controller->apploader_apps ?
            apploader_list_item_by_id(controller->apploader_apps, app_id) : NULL;
    if (!app || (app->hidden && !controller->show_hidden_apps)) {
        int count = adapter_item_count(controller->applist, controller->apploader_apps);
        if (count <= 0) return;
        app = &controller->apploader_apps->items[0];
    }
    lv_label_set_text(controller->hero_title, app->base.name);
    int current = pcmanager_server_current_app(pcmanager, &controller->uuid);
    lv_label_set_text_static(controller->launch_label, current == app->base.id ? locstr("Resume") : locstr("Play"));
    if (current && current != app->base.id) lv_obj_add_state(controller->launch_btn, LV_STATE_DISABLED);
    else lv_obj_clear_state(controller->launch_btn, LV_STATE_DISABLED);
    if (controller->featured_id == app->base.id || !controller->coverloader) return;
    controller->featured_id = app->base.id;
    lv_coord_t width = lv_obj_get_width(controller->hero_image);
    lv_coord_t height = lv_obj_get_height(controller->hero_image);
    coverloader_display_backdrop(controller->coverloader, &controller->uuid, app->base.id,
                                 controller->hero_image, width, height);
}

static void featured_click(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    const apploader_item_t *app = apploader_list_item_by_id(controller->apploader_apps, controller->featured_id);
    int running = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (!app || (running && running != app->base.id)) return;
    launcher_launch_game(controller, app);
}

static void featured_options(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    if (!controller->featured_id) return;
    appitem_viewholder_t holder = {.app_id = controller->featured_id};
    open_context_menu(controller, &holder);
}

static void featured_key(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    uint32_t key = lv_event_get_key(event);
    lv_obj_t *target = lv_event_get_target(event);
    if (key == LV_KEY_DOWN) {
        lv_group_focus_obj(controller->applist);
        lv_gridview_focus(controller->applist, controller->focus_backup);
    } else if (key == LV_KEY_UP) {
        launcher_focus_navigation((launcher_fragment_t *) lv_fragment_get_parent(&controller->base));
    } else if (key == LV_KEY_RIGHT && target == controller->launch_btn) {
        lv_group_focus_obj(controller->options_btn);
    } else if (key == LV_KEY_LEFT && target == controller->options_btn) {
        if (!lv_obj_has_state(controller->launch_btn, LV_STATE_DISABLED)) lv_group_focus_obj(controller->launch_btn);
    } else if (key == LV_KEY_DEL) {
        featured_options(event);
    }
}

static void shelf_key(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    if (lv_event_get_target(event) != controller->applist) return;
    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_UP) {
        apps_focus_primary(controller);
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_DEL) {
        featured_options(event);
    }
}

static void shelf_focused(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *item = lv_event_get_target(event);
    if (lv_obj_get_parent(item) != controller->applist) return;
    const appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    if (holder) set_featured(controller, holder->app_id);
}

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (controller->quit_progress) {
        lv_msgbox_close(controller->quit_progress);
        controller->quit_progress = NULL;
    }
    lv_gridview_rebind(controller->applist);
    if (result == GS_OK) {
        return;
    }
    static const char *btn_texts[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Unable to quit game"),
                                             locstr("Please make sure you are quitting with the same client."),
                                             btn_texts, false);
    lv_obj_add_event_cb(dialog, quit_dialog_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_center(dialog);
}

static void quit_dialog_cb(lv_event_t *event) {
    lv_obj_t *dialog = lv_event_get_current_target(event);
    lv_msgbox_close_async(dialog);
}

static void actions_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    uint16_t index = lv_btnmatrix_get_selected_btn(controller->actions);
    const action_cb_t *actions = lv_obj_get_user_data(controller->actions);
    actions[index](controller, controller->actions, index);
}

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_send_wol(pcmanager, &controller->uuid, send_wol_cb, NULL);
}

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(buttons);
    LV_UNUSED(index);
    pair_dialog_open(&controller->uuid);
}

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder) {
    const apploader_item_t *app = apploader_list_item_by_id(fragment->apploader_apps, holder->app_id);
    assert(app != NULL);
    lv_obj_t *msgbox = lv_msgbox_create(NULL, app->base.name, NULL, NULL, false);
    /* A menu can outlive the recycled shelf item (or a temporary featured holder). */
    lv_obj_set_user_data(msgbox, (void *) (intptr_t) holder->app_id);
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(content, context_menu_cancel_cb, LV_EVENT_CANCEL, fragment);
    lv_obj_add_event_cb(content, context_menu_click_cb, LV_EVENT_SHORT_CLICKED, fragment);

    int currentId = pcmanager_server_current_app(pcmanager, &fragment->uuid);
    if (!currentId || currentId == app->base.id) {
        lv_obj_t *start_btn = lv_list_add_btn(content, NULL,
                                              currentId == app->base.id ? locstr("Resume streaming")
                                                                        : locstr("Start streaming"));
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(start_btn, launcher_launch_game);
    }

    if (currentId) {
        lv_obj_t *quit_btn = lv_list_add_btn(content, NULL, locstr("Stop streaming"));
        lv_obj_add_flag(quit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(quit_btn, launcher_quit_game);
    }
    lv_obj_t *fav_btn = lv_list_add_btn(content, NULL, app->fav ? locstr("Unstar") : locstr("Star"));
    lv_obj_add_flag(fav_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(fav_btn, launcher_toggle_fav);

    lv_obj_t *hide_btn = lv_list_add_btn(content, NULL, app->hidden ? locstr("Unhide") : locstr("Hide"));
    lv_obj_add_flag(hide_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(hide_btn, launcher_toggle_hidden);

    lv_obj_t *info_btn = lv_list_add_btn(content, NULL, locstr("Info"));
    lv_obj_add_flag(info_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(info_btn, app_detail_dialog);

    lv_obj_t *cancel_btn = lv_list_add_btn(content, NULL, locstr("Cancel"));
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(msgbox);
}

static void context_menu_cancel_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target->parent != lv_event_get_current_target(e)) { return; }
    lv_msgbox_close(lv_event_get_current_target(e)->parent);
}

static void context_menu_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *current_target = lv_event_get_current_target(e);
    if (target->parent != current_target) { return; }
    lv_obj_t *mbox = lv_event_get_current_target(e)->parent;
    apps_fragment_t *self = lv_event_get_user_data(e);
    int app_id = (int) (intptr_t) lv_obj_get_user_data(mbox);
    const apploader_item_t *app = apploader_list_item_by_id(self->apploader_apps, app_id);
    if (!app) {
        lv_msgbox_close_async(mbox);
        return;
    }
    if (lv_obj_get_user_data(target) == launcher_quit_game) {
        launcher_quit_game(self);
    } else if (lv_obj_get_user_data(target) == launcher_launch_game) {
        launcher_launch_game(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_fav) {
        launcher_toggle_fav(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_hidden) {
        launcher_toggle_hidden(self, app);
    } else if (lv_obj_get_user_data(target) == app_detail_dialog) {
        app_detail_dialog(self, app);
    }
    lv_msgbox_close_async(mbox);
}

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app) {
    LV_UNUSED(fragment);
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, app->base.name, "text", btn_txts, false);
    lv_obj_t *msgobj = lv_msgbox_get_text(msgbox);
    lv_label_set_text_fmt(msgobj,
                          "ID: %d\n"
                          "Support HDR: %s",
                          app->base.id, app->base.hdr ? "Yes" : "No");
    lv_obj_center(msgbox);
    lv_obj_add_event_cb(msgbox, app_detail_click_cb, LV_EVENT_CLICKED, NULL);
}

static void app_detail_click_cb(lv_event_t *event) {
    lv_msgbox_close(lv_event_get_current_target(event));
}

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks) {
    lv_btnmatrix_set_map(controller->actions, labels);
    int num_actions = 0;
    for (int i = 0; labels[i][0] != '\0'; i++) {
        num_actions++;
    }
    lv_obj_set_style_min_width(controller->actions, LV_PCT(20 * num_actions), 0);
    lv_obj_set_user_data(controller->actions, (void *) callbacks);
}


static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list,
                                                          const apploader_list_t *new_list, int *num_changes) {
    if (old_list == NULL && new_list == NULL) {
        *num_changes = 0;
        return NULL;
    } else if ((old_list != NULL) != (new_list != NULL)) {
        *num_changes = -1;
        return NULL;
    } else if (old_list->count != new_list->count) {
        *num_changes = -1;
        return NULL;
    }
    for (int i = 0; i < old_list->count; i++) {
        if (old_list->items[i].base.id != new_list->items[i].base.id) {
            *num_changes = -1;
            return NULL;
        }
    }
    *num_changes = 0;
    return NULL;
}
