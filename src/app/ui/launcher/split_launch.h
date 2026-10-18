#pragma once

#include <lvgl.h>
#include <stdbool.h>

#include "client.h"
#include "uuidstr.h"

typedef struct app_t app_t;

/* Adds "Split screen with <host>" to an app's context menu for every other paired, online host */
void split_launch_add_menu_items(lv_obj_t *content, const uuidstr_t *uuid);

/* Starts split screen if btn is one of those items: the app on the left half, the partner host's app with the
 * same name on the right (falling back to Steam Big Picture, then Desktop) */
bool split_launch_handle_click(lv_obj_t *btn, app_t *global, const uuidstr_t *uuid, const APP_LIST *app);
