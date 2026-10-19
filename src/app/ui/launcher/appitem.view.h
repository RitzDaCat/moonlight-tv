#pragma once

#include <backend/apploader/apploader.h>
#include "lvgl.h"
#include "client.h"

#include "lv_sdl_img.h"
#include "apps.controller.h"

typedef struct app_fonts_t app_fonts_t;

lv_obj_t *appitem_view(appitem_styles_t *styles, lv_coord_t width, lv_coord_t height, lv_obj_t *parent);

/* Full-bleed art behind the hero; hidden until its image arrives. */
lv_obj_t *appitem_backdrop_view(appitem_styles_t *styles, lv_obj_t *parent);

/* Room below a poster for its title. */
lv_coord_t appitem_caption_height(lv_obj_t *ref);

void appitem_style_init(appitem_styles_t *style);

void appitem_style_deinit(appitem_styles_t *style);
