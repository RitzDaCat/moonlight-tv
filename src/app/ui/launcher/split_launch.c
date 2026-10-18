#include "split_launch.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "backend/pcmanager.h"
#include "launcher.controller.h"
#include "ui/streaming/streaming.controller.h"
#include "util/i18n.h"

static struct {
    bool active;
    uuidstr_t left_uuid, right_uuid;
    int app_id;
    char *app_name;
    bool app_hdr;
    char banner[256];
} pending;

static void menu_item_deleted_cb(lv_event_t *e);

static void pending_clear();

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

bool split_launch_handle_click(lv_obj_t *btn, const uuidstr_t *uuid, const APP_LIST *app) {
    if (!lv_obj_has_flag(btn, LV_OBJ_FLAG_USER_1)) {
        return false;
    }
    pending_clear();
    pending.left_uuid = *uuid;
    pending.right_uuid = *(const uuidstr_t *) lv_obj_get_user_data(btn);
    pending.app_id = app->id;
    pending.app_name = strdup(app->name);
    pending.app_hdr = app->hdr;
    const pclist_t *right = pcmanager_node(pcmanager, &pending.right_uuid);
    snprintf(pending.banner, sizeof(pending.banner), locstr("Split screen: pick a game for %s. %s plays on the left."),
             right != NULL ? right->server->hostname : "", pending.app_name);
    pending.active = true;
    launcher_select_server(launcher_instance(), &pending.right_uuid);
    return true;
}

const char *split_launch_pick_banner(const uuidstr_t *host) {
    if (!pending.active || !uuidstr_t_equals_t(&pending.right_uuid, host)) {
        return NULL;
    }
    return pending.banner;
}

bool split_launch_pick(app_t *global, const uuidstr_t *host, const APP_LIST *app) {
    if (split_launch_pick_banner(host) == NULL) {
        return false;
    }
    streaming_scene_arg_t args = {
            .global = global,
            .uuid = pending.left_uuid,
            .app = {.name = pending.app_name, .id = pending.app_id, .hdr = pending.app_hdr},
            .split = true,
            .split_uuid = pending.right_uuid,
            .split_app = *app,
    };
    app_ui_t *ui = &global->ui;
    // The scene copies what it needs while being created
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    pending_clear();
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
    return true;
}

void split_launch_cancel(const uuidstr_t *host) {
    if (split_launch_pick_banner(host) != NULL) {
        pending_clear();
    }
}

static void menu_item_deleted_cb(lv_event_t *e) {
    free(lv_obj_get_user_data(lv_event_get_target(e)));
}

static void pending_clear() {
    free(pending.app_name);
    pending.app_name = NULL;
    pending.active = false;
}
