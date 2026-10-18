#pragma once

#include <lvgl.h>
#include <stdbool.h>

#include "client.h"
#include "uuidstr.h"

typedef struct app_t app_t;

/* Adds "Split screen with <host>" to an app's context menu for every other host not known to be
 * offline or unpaired */
void split_launch_add_menu_items(lv_obj_t *content, const uuidstr_t *uuid);

/* If btn is one of those items: remembers the app for the left half and opens the partner host's games,
 * where the right half's game gets picked like any normal launch */
bool split_launch_handle_click(lv_obj_t *btn, const uuidstr_t *uuid, const APP_LIST *app);

/* Banner text while host's games are shown to pick the right half, NULL otherwise */
const char *split_launch_pick_banner(const uuidstr_t *host);

/* Starts split screen if host's games are being picked for the right half */
bool split_launch_pick(app_t *global, const uuidstr_t *host, const APP_LIST *app);

/* Leaves picking mode if it was for host */
void split_launch_cancel(const uuidstr_t *host);
