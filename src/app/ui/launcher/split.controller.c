#include "split.controller.h"

#include "app.h"
#include "appitem.view.h"
#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include "backend/pcmanager.h"
#include "ui/root.h"
#include "ui/streaming/streaming.controller.h"

#include "lv_gridview.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/util/lv_app_utils.h"
#include "util/i18n.h"

#define SPLIT_COLUMNS 3

typedef struct split_controller_t split_controller_t;

typedef struct split_side_t {
    split_controller_t *parent;
    int index;
    bool has_host;
    uuidstr_t uuid;
    apploader_t *loader;
    apploader_cb_t loader_cb;
    apploader_list_t *apps;
    bool load_failed;
    int picked_app;

    lv_obj_t *status, *hosts, *grid, *spinner;
    lv_group_t *group;
    lv_coord_t col_width, col_height;
} split_side_t;

struct split_controller_t {
    lv_fragment_t base;
    app_t *global;
    coverloader_t *coverloader;
    appitem_styles_t appitem_style;
    split_side_t sides[2];
    int active_side;
};

static void constructor(lv_fragment_t *self, void *args);

static void destructor(lv_fragment_t *self);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void obj_created(lv_fragment_t *self, lv_obj_t *obj);

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj);

static void obj_deleted(lv_fragment_t *self, lv_obj_t *obj);

static lv_obj_t *side_create(split_side_t *side, lv_obj_t *parent);

static void side_set_host(split_side_t *side, const uuidstr_t *uuid);

static void side_load(split_side_t *side);

static void side_update(split_side_t *side);

static void hosts_update(split_side_t *side);

static void set_active_side(split_controller_t *controller, int index);

static void try_start(split_controller_t *controller);

static void close_cb(lv_event_t *e);

static void cancel_cb(lv_event_t *e);

static void host_click_cb(lv_event_t *e);

static void host_key_cb(lv_event_t *e);

static void grid_click_cb(lv_event_t *e);

static void grid_key_cb(lv_event_t *e);

static void focus_cb(lv_event_t *e);

static void on_host_updated(const uuidstr_t *uuid, void *userdata);

static void appload_started(void *userdata);

static void appload_loaded(apploader_list_t *apps, void *userdata);

static void appload_errored(int code, const char *error, void *userdata);

static int adapter_item_count(lv_obj_t *grid, void *data);

static lv_obj_t *adapter_create_view(lv_obj_t *parent);

static void adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position);

const lv_fragment_class_t split_controller_class = {
        .constructor_cb = constructor,
        .destructor_cb = destructor,
        .create_obj_cb = create_obj,
        .obj_created_cb = obj_created,
        .obj_will_delete_cb = obj_will_delete,
        .obj_deleted_cb = obj_deleted,
        .instance_size = sizeof(split_controller_t),
};

static const lv_gridview_adapter_t apps_adapter = {
        .item_count = adapter_item_count,
        .create_view = adapter_create_view,
        .bind_view = adapter_bind_view,
};

static const pcmanager_listener_t pc_listeners = {
        .updated = on_host_updated,
};

static split_controller_t *current_instance = NULL;

void split_controller_route_input(int gs_id) {
    split_controller_t *controller = current_instance;
    if (controller == NULL) {
        return;
    }
    int index = gs_id < 0 ? 0 : gs_id & 1;
    if (controller->sides[index].picked_app != 0 && controller->sides[!index].picked_app == 0) {
        index = !index;
    }
    set_active_side(controller, index);
}

static void constructor(lv_fragment_t *self, void *args) {
    split_controller_t *controller = (split_controller_t *) self;
    controller->global = args;
    appitem_style_init(&controller->appitem_style);
    const pclist_t *first = NULL, *selected = NULL;
    for (const pclist_t *node = pcmanager_servers(pcmanager); node != NULL; node = node->next) {
        if (first == NULL) {
            first = node;
        }
        if (node->selected) {
            selected = node;
        }
    }
    const pclist_t *left = selected != NULL ? selected : first;
    const pclist_t *right = NULL;
    for (const pclist_t *node = pcmanager_servers(pcmanager); node != NULL && right == NULL; node = node->next) {
        if (node != left) {
            right = node;
        }
    }
    const pclist_t *defaults[2] = {left, right};
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        side->parent = controller;
        side->index = i;
        side->loader_cb.start = appload_started;
        side->loader_cb.data = appload_loaded;
        side->loader_cb.error = appload_errored;
        if (defaults[i] != NULL) {
            side->has_host = true;
            side->uuid = defaults[i]->id;
            side->loader = apploader_create(controller->global, &side->uuid, &side->loader_cb, side);
        }
    }
}

static void destructor(lv_fragment_t *self) {
    split_controller_t *controller = (split_controller_t *) self;
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        if (side->loader != NULL) {
            apploader_destroy(side->loader);
        }
        if (side->apps != NULL) {
            apploader_list_free(side->apps);
        }
    }
    appitem_style_deinit(&controller->appitem_style);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    split_controller_t *controller = (split_controller_t *) self;
    lv_obj_t *win = lv_win_create(container, LV_DPX(50));

    lv_obj_t *header = lv_win_get_header(win);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(header, lv_dpx(10), 0);
    lv_obj_set_style_pad_gap(header, 0, 0);
    lv_obj_set_style_pad_left(header, 0, 0);
    lv_obj_set_style_pad_right(header, lv_dpx(20), 0);
    lv_obj_set_style_bg_color(header, lv_color_darken(lv_color_hex(0x2f3237), 4), 0);

    lv_obj_t *icon = lv_img_create(header);
    lv_obj_set_size(icon, LV_DPX(NAV_WIDTH_COLLAPSED), LV_DPX(50));
    lv_obj_set_style_pad_hor(icon, LV_DPX((NAV_WIDTH_COLLAPSED - NAV_LOGO_SIZE) / 2), 0);
    lv_obj_set_style_pad_ver(icon, LV_DPX((50 - NAV_LOGO_SIZE) / 2), 0);
    lv_img_set_src(icon, ui_logo_src());

    lv_obj_t *title = lv_win_add_title(win, locstr("Split screen"));
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(win), 0);

    lv_obj_t *close_btn = lv_win_add_btn(win, MAT_SYMBOL_CLOSE, lv_dpx(28));
    lv_btn_set_icon_font(close_btn, lv_theme_moonlight_get_iconfont_normal(close_btn));
    lv_obj_set_style_width(close_btn, lv_dpx(28), 0);
    lv_obj_set_style_height(close_btn, lv_dpx(28), 0);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, controller);

    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_gap(content, LV_DPX(2), 0);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 2; i++) {
        side_create(&controller->sides[i], content);
    }
    return win;
}

static lv_obj_t *side_create(split_side_t *side, lv_obj_t *parent) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_gap(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_lighten(lv_color_black(), 30), 0);

    lv_obj_t *top = lv_obj_create(panel);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(top, lv_dpx(24), 0);
    lv_obj_set_style_pad_top(top, lv_dpx(16), 0);
    lv_obj_set_style_pad_gap(top, lv_dpx(12), 0);

    lv_obj_t *player = lv_label_create(top);
    lv_obj_set_style_text_font(player, lv_theme_get_font_large(player), 0);
    lv_label_set_text_static(player, side->index == 0 ? locstr("Player 1") : locstr("Player 2"));

    side->status = lv_label_create(top);
    lv_obj_set_flex_grow(side->status, 1);
    lv_obj_set_style_text_align(side->status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(side->status, LV_LABEL_LONG_DOT);

    side->hosts = lv_obj_create(panel);
    lv_obj_remove_style_all(side->hosts);
    lv_obj_set_size(side->hosts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side->hosts, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(side->hosts, lv_dpx(24), 0);
    lv_obj_set_style_pad_ver(side->hosts, lv_dpx(12), 0);
    lv_obj_set_style_pad_gap(side->hosts, lv_dpx(8), 0);

    lv_obj_t *grid = side->grid = lv_gridview_create(panel);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_all(grid, lv_dpx(24), 0);
    lv_obj_set_style_pad_gap(grid, lv_dpx(24), 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_border_side(grid, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_user_data(grid, side);
    lv_gridview_set_adapter(grid, &apps_adapter);

    side->spinner = lv_spinner_create(panel, 1000, 60);
    lv_obj_add_flag(side->spinner, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(side->spinner, lv_dpx(60), lv_dpx(60));
    lv_obj_center(side->spinner);
    return panel;
}

static void obj_created(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    split_controller_t *controller = (split_controller_t *) self;
    controller->coverloader = coverloader_new(controller->global);
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        side->group = lv_group_create();
        lv_group_set_wrap(side->group, false);
        lv_group_add_obj(side->group, side->grid);
        lv_obj_add_event_cb(side->grid, grid_click_cb, LV_EVENT_SHORT_CLICKED, side);
        lv_obj_add_event_cb(side->grid, grid_key_cb, LV_EVENT_KEY, side);
        lv_obj_add_event_cb(side->grid, cancel_cb, LV_EVENT_CANCEL, side);
        lv_obj_add_event_cb(side->grid, focus_cb, LV_EVENT_FOCUSED, side);

        lv_obj_update_layout(side->grid);
        lv_coord_t width = lv_obj_get_content_width(side->grid);
        side->col_width = (width - lv_obj_get_style_pad_column(side->grid, 0) * (SPLIT_COLUMNS - 1)) / SPLIT_COLUMNS;
        side->col_height = side->col_width / 3 * 4;
        lv_gridview_set_config(side->grid, SPLIT_COLUMNS, side->col_height, LV_GRID_ALIGN_CENTER,
                               LV_GRID_ALIGN_CENTER);
        hosts_update(side);
    }
    controller->appitem_style.defcover_src.header.w = controller->sides[0].col_width;
    controller->appitem_style.defcover_src.header.h = controller->sides[0].col_height;
    current_instance = controller;
    pcmanager_register_listener(pcmanager, &pc_listeners, controller);
    controller->active_side = -1;
    set_active_side(controller, 0);
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        if (side->apps != NULL) {
            lv_gridview_set_data(side->grid, side->apps);
        }
        side_load(side);
    }
}

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    split_controller_t *controller = (split_controller_t *) self;
    for (int i = 0; i < 2; i++) {
        if (controller->sides[i].loader != NULL) {
            apploader_cancel(controller->sides[i].loader);
        }
    }
}

static void obj_deleted(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    split_controller_t *controller = (split_controller_t *) self;
    current_instance = NULL;
    pcmanager_unregister_listener(pcmanager, &pc_listeners);
    app_input_set_group(&controller->global->ui.input, NULL);
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        lv_group_del(side->group);
        side->group = NULL;
        side->grid = side->hosts = side->status = side->spinner = NULL;
    }
    coverloader_unref(controller->coverloader);
    controller->coverloader = NULL;
}

static void set_active_side(split_controller_t *controller, int index) {
    if (controller->active_side == index) {
        return;
    }
    controller->active_side = index;
    app_input_set_group(&controller->global->ui.input, controller->sides[index].group);
}

static void hosts_update(split_side_t *side) {
    const split_side_t *other = &side->parent->sides[!side->index];
    lv_group_remove_all_objs(side->group);
    lv_obj_clean(side->hosts);
    for (const pclist_t *node = pcmanager_servers(pcmanager); node != NULL; node = node->next) {
        lv_obj_t *btn = lv_btn_create(side->hosts);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, node->server->hostname);
        bool mine = side->has_host && uuidstr_t_equals_t(&node->id, &side->uuid);
        bool taken = other->has_host && uuidstr_t_equals_t(&node->id, &other->uuid);
        if (mine) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        }
        if (taken) {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        }
        lv_obj_set_user_data(btn, (void *) &node->id);
        lv_obj_add_event_cb(btn, host_click_cb, LV_EVENT_CLICKED, side);
        lv_obj_add_event_cb(btn, host_key_cb, LV_EVENT_KEY, side);
        lv_obj_add_event_cb(btn, cancel_cb, LV_EVENT_CANCEL, side);
        lv_group_add_obj(side->group, btn);
    }
    lv_group_add_obj(side->group, side->grid);
    lv_group_focus_obj(side->grid);
}

static void side_set_host(split_side_t *side, const uuidstr_t *uuid) {
    if (side->has_host && uuidstr_t_equals_t(&side->uuid, uuid)) {
        return;
    }
    if (side->loader != NULL) {
        apploader_destroy(side->loader);
    }
    if (side->apps != NULL) {
        apploader_list_free(side->apps);
        side->apps = NULL;
    }
    side->has_host = true;
    side->uuid = *uuid;
    side->picked_app = 0;
    side->load_failed = false;
    side->loader = apploader_create(side->parent->global, &side->uuid, &side->loader_cb, side);
    lv_gridview_set_data(side->grid, NULL);
    for (int i = 0; i < 2; i++) {
        hosts_update(&side->parent->sides[i]);
    }
    side_load(side);
}

static void side_load(split_side_t *side) {
    if (!side->has_host) {
        side_update(side);
        return;
    }
    const SERVER_STATE *state = pcmanager_state(pcmanager, &side->uuid);
    if (state != NULL && state->code == SERVER_STATE_AVAILABLE) {
        if (side->apps == NULL && apploader_state(side->loader) != APPLOADER_STATE_LOADING) {
            apploader_load(side->loader);
        }
    } else if (state == NULL || state->code != SERVER_STATE_QUERYING) {
        pcmanager_request_update(pcmanager, &side->uuid, NULL, NULL);
    }
    side_update(side);
}

static void side_update(split_side_t *side) {
    const SERVER_STATE *state = side->has_host ? pcmanager_state(pcmanager, &side->uuid) : NULL;
    bool loading = false;
    lv_obj_set_style_text_color(side->status, lv_color_white(), 0);
    if (!side->has_host) {
        lv_label_set_text_static(side->status, locstr("Add another computer to play split screen"));
    } else if (state == NULL || state->code == SERVER_STATE_NONE || state->code == SERVER_STATE_QUERYING) {
        lv_label_set_text_static(side->status, locstr("Connecting..."));
        loading = true;
    } else if (state->code == SERVER_STATE_NOT_PAIRED) {
        lv_label_set_text_static(side->status, locstr("Not paired"));
    } else if (state->code != SERVER_STATE_AVAILABLE) {
        lv_label_set_text_static(side->status, locstr("Offline"));
    } else if (side->load_failed) {
        lv_label_set_text_static(side->status, locstr("Failed to load apps"));
    } else if (side->apps == NULL) {
        lv_label_set_text_static(side->status, locstr("Loading..."));
        loading = true;
    } else if (side->picked_app == 0) {
        lv_label_set_text_static(side->status, locstr("Pick a game"));
    } else {
        const apploader_item_t *app = apploader_list_item_by_id(side->apps, side->picked_app);
        lv_label_set_text_fmt(side->status, locstr("Ready: %s"), app != NULL ? app->base.name : "");
        lv_obj_set_style_text_color(side->status, lv_palette_lighten(LV_PALETTE_GREEN, 2), 0);
    }
    if (loading) {
        lv_obj_clear_flag(side->spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(side->spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void try_start(split_controller_t *controller) {
    split_side_t *left = &controller->sides[0], *right = &controller->sides[1];
    if (left->picked_app == 0 || right->picked_app == 0) {
        return;
    }
    const apploader_item_t *left_app = apploader_list_item_by_id(left->apps, left->picked_app);
    const apploader_item_t *right_app = apploader_list_item_by_id(right->apps, right->picked_app);
    left->picked_app = right->picked_app = 0;
    if (left_app == NULL || right_app == NULL) {
        return;
    }
    streaming_scene_arg_t args = {
            .global = controller->global,
            .uuid = left->uuid,
            .app = left_app->base,
            .split = true,
            .split_uuid = right->uuid,
            .split_app = right_app->base,
    };
    app_ui_t *ui = &controller->global->ui;
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
}

static void close_cb(lv_event_t *e) {
    split_controller_t *controller = lv_event_get_user_data(e);
    lv_fragment_del(&controller->base);
}

static void cancel_cb(lv_event_t *e) {
    split_side_t *side = lv_event_get_user_data(e);
    if (side->picked_app != 0) {
        side->picked_app = 0;
        lv_gridview_rebind(side->grid);
        side_update(side);
        return;
    }
    lv_fragment_del(&side->parent->base);
}

static void host_click_cb(lv_event_t *e) {
    split_side_t *side = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    side_set_host(side, lv_obj_get_user_data(btn));
}

static void host_key_cb(lv_event_t *e) {
    split_side_t *side = lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
            lv_group_focus_prev(side->group);
            break;
        case LV_KEY_RIGHT:
            if (lv_obj_get_index(lv_event_get_current_target(e)) + 1 < (int) lv_obj_get_child_cnt(side->hosts)) {
                lv_group_focus_next(side->group);
            }
            break;
        case LV_KEY_DOWN:
            lv_group_focus_obj(side->grid);
            break;
        default:
            break;
    }
}

static void grid_key_cb(lv_event_t *e) {
    // Only reached when the grid couldn't move focus within itself
    split_side_t *side = lv_event_get_user_data(e);
    if (lv_event_get_key(e) == LV_KEY_UP && lv_obj_get_child_cnt(side->hosts) > 0) {
        lv_gridview_focus(side->grid, -1);
        lv_obj_t *checked = NULL;
        for (int i = 0, n = (int) lv_obj_get_child_cnt(side->hosts); i < n; i++) {
            lv_obj_t *btn = lv_obj_get_child(side->hosts, i);
            if (checked == NULL || lv_obj_has_state(btn, LV_STATE_CHECKED)) {
                checked = btn;
            }
        }
        lv_group_focus_obj(checked);
    }
}

static void focus_cb(lv_event_t *e) {
    split_side_t *side = lv_event_get_user_data(e);
    if (side->apps != NULL && lv_gridview_get_focused_index(side->grid) < 0) {
        lv_gridview_focus(side->grid, 0);
    }
}

static void grid_click_cb(lv_event_t *e) {
    split_side_t *side = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (lv_obj_get_parent(target) != side->grid) {
        return;
    }
    appitem_viewholder_t *holder = lv_obj_get_user_data(target);
    side->picked_app = holder->app_id;
    lv_gridview_rebind(side->grid);
    side_update(side);
    split_controller_t *controller = side->parent;
    if (controller->sides[!side->index].picked_app == 0) {
        set_active_side(controller, !side->index);
    }
    try_start(controller);
}

static void on_host_updated(const uuidstr_t *uuid, void *userdata) {
    split_controller_t *controller = userdata;
    for (int i = 0; i < 2; i++) {
        split_side_t *side = &controller->sides[i];
        if (side->has_host && uuidstr_t_equals_t(&side->uuid, uuid)) {
            side_load(side);
        }
    }
}

static void appload_started(void *userdata) {
    split_side_t *side = userdata;
    side->load_failed = false;
    if (side->grid != NULL) {
        side_update(side);
    }
}

static void appload_loaded(apploader_list_t *apps, void *userdata) {
    split_side_t *side = userdata;
    if (side->apps != NULL) {
        apploader_list_free(side->apps);
    }
    side->apps = apps;
    if (side->grid != NULL) {
        lv_gridview_set_data(side->grid, apps);
        if (lv_group_get_focused(side->group) == side->grid) {
            lv_gridview_focus(side->grid, 0);
        }
        side_update(side);
    }
}

static void appload_errored(int code, const char *error, void *userdata) {
    LV_UNUSED(code);
    LV_UNUSED(error);
    split_side_t *side = userdata;
    side->load_failed = true;
    if (side->grid != NULL) {
        side_update(side);
    }
}

static int adapter_item_count(lv_obj_t *grid, void *data) {
    LV_UNUSED(grid);
    if (data == NULL) {
        return 0;
    }
    const apploader_list_t *list = data;
    int count = LV_MIN(list->count, 255 * SPLIT_COLUMNS);
    for (int i = 0; i < count; i++) {
        if (list->items[i].hidden) {
            return i;
        }
    }
    return count;
}

static lv_obj_t *adapter_create_view(lv_obj_t *parent) {
    split_side_t *side = lv_obj_get_user_data(parent);
    lv_obj_t *item = appitem_view(&side->parent->appitem_style, side->col_width, side->col_height, parent);
    lv_obj_set_style_outline_color(item, lv_palette_main(LV_PALETTE_GREEN), LV_STATE_CHECKED);
    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_CHECKED);
    return item;
}

static void adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    split_side_t *side = lv_obj_get_user_data(grid);
    const apploader_list_t *list = data;
    const apploader_item_t *app = &list->items[position];
    appitem_viewholder_t *holder = lv_obj_get_user_data(item_view);
    coverloader_display(side->parent->coverloader, &side->uuid, app->base.id, item_view, side->col_width,
                        side->col_height);
    lv_label_set_text(holder->title, app->base.name);
    holder->app_id = app->base.id;
    holder->fav = app->fav;
    if (side->picked_app == app->base.id) {
        lv_obj_add_state(item_view, LV_STATE_CHECKED);
        lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_state(item_view, LV_STATE_CHECKED);
        lv_obj_add_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}
