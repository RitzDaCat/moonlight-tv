#pragma once

#include "lvgl.h"

typedef struct app_t app_t;

/* Split screen page: each half picks a computer and a game, both picked starts streaming side by side.
 * Arg is app_t*. */
extern const lv_fragment_class_t split_controller_class;

/* Points navigation at the half owned by the gamepad (gs_id even: left, odd: right, -1: remote/keyboard).
 * Once a half is ready, everyone navigates the other one, so a single controller can set up both. */
void split_controller_route_input(int gs_id);
