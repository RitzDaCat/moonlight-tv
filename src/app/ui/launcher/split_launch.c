#include "split_launch.h"

#include <string.h>

#include "app.h"
#include "backend/apploader/apploader.h"
#include "backend/pcmanager.h"
#include "ui/common/progress_dialog.h"
#include "ui/streaming/streaming.controller.h"
#include "util/i18n.h"
#include "lvgl/util/lv_app_utils.h"

typedef struct split_launch_t {
    app_t *global;
    uuidstr_t uuid, split_uuid;
    int app_id;
    char *app_name;
    bool app_hdr;
    apploader_t *loader;
    lv_obj_t *progress;
} split_launch_t;

static void menu_item_deleted_cb(lv_event_t *e);

static void loader_start(void *userdata);

static void loader_data(apploader_list_t *apps, void *userdata);

static void loader_error(int code, const char *error, void *userdata);

static void split_launch_free(split_launch_t *launch);

static void error_msgbox_cb(lv_event_t *e);

static const apploader_cb_t loader_callbacks = {
        .start = loader_start,
        .data = loader_data,
        .error = loader_error,
};

void split_launch_add_menu_items(lv_obj_t *content, const uuidstr_t *uuid) {
    for (const pclist_t *node = pcmanager_servers(pcmanager); node != NULL; node = node->next) {
        if (uuidstr_t_equals_t(&node->id, uuid) || node->state.code != SERVER_STATE_AVAILABLE) {
            continue;
        }
        lv_obj_t *btn = lv_list_add_btn(content, NULL, NULL);
        lv_label_set_text_fmt(lv_obj_get_child(btn, 0), locstr("Split screen with %s"), node->server->hostname);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_USER_1);
        uuidstr_t *split_uuid = malloc(sizeof(uuidstr_t));
        *split_uuid = node->id;
        lv_obj_set_user_data(btn, split_uuid);
        lv_obj_add_event_cb(btn, menu_item_deleted_cb, LV_EVENT_DELETE, NULL);
    }
}

bool split_launch_handle_click(lv_obj_t *btn, app_t *global, const uuidstr_t *uuid, const APP_LIST *app) {
    if (!lv_obj_has_flag(btn, LV_OBJ_FLAG_USER_1)) {
        return false;
    }
    split_launch_t *launch = calloc(1, sizeof(split_launch_t));
    launch->global = global;
    launch->uuid = *uuid;
    launch->split_uuid = *(const uuidstr_t *) lv_obj_get_user_data(btn);
    launch->app_id = app->id;
    launch->app_name = strdup(app->name);
    launch->app_hdr = app->hdr;
    launch->loader = apploader_create(global, &launch->split_uuid, &loader_callbacks, launch);
    apploader_load(launch->loader);
    return true;
}

static void menu_item_deleted_cb(lv_event_t *e) {
    free(lv_obj_get_user_data(lv_event_get_target(e)));
}

static void loader_start(void *userdata) {
    split_launch_t *launch = userdata;
    if (launch->progress == NULL) {
        launch->progress = progress_dialog_create(locstr("Loading..."));
    }
}

static const APP_LIST *pick_split_app(const apploader_list_t *apps, const char *name) {
    static const char *fallbacks[] = {"Steam Big Picture", "Desktop"};
    for (size_t i = 0; i < apps->count; i++) {
        if (strcmp(apps->items[i].base.name, name) == 0) {
            return &apps->items[i].base;
        }
    }
    for (size_t f = 0; f < sizeof(fallbacks) / sizeof(fallbacks[0]); f++) {
        for (size_t i = 0; i < apps->count; i++) {
            if (strcmp(apps->items[i].base.name, fallbacks[f]) == 0) {
                return &apps->items[i].base;
            }
        }
    }
    return apps->count > 0 ? &apps->items[0].base : NULL;
}

static void loader_data(apploader_list_t *apps, void *userdata) {
    split_launch_t *launch = userdata;
    const APP_LIST *split_app = pick_split_app(apps, launch->app_name);
    if (split_app == NULL) {
        apploader_list_free(apps);
        loader_error(0, locstr("The other computer has no apps."), launch);
        return;
    }
    streaming_scene_arg_t args = {
            .global = launch->global,
            .uuid = launch->uuid,
            .app = {.name = launch->app_name, .id = launch->app_id, .hdr = launch->app_hdr},
            .split = true,
            .split_uuid = launch->split_uuid,
            .split_app = *split_app,
    };
    app_ui_t *ui = &launch->global->ui;
    // The scene copies what it needs while being created
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
    apploader_list_free(apps);
    lv_async_call((lv_async_cb_t) split_launch_free, launch);
}

static void loader_error(int code, const char *error, void *userdata) {
    (void) code;
    split_launch_t *launch = userdata;
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Split screen"), error, btn_txts, false);
    lv_obj_add_event_cb(msgbox, error_msgbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
    lv_async_call((lv_async_cb_t) split_launch_free, launch);
}

static void error_msgbox_cb(lv_event_t *e) {
    lv_msgbox_close_async(lv_event_get_current_target(e));
}

static void split_launch_free(split_launch_t *launch) {
    if (launch->progress != NULL) {
        lv_msgbox_close(launch->progress);
    }
    apploader_destroy(launch->loader);
    free(launch->app_name);
    free(launch);
}
