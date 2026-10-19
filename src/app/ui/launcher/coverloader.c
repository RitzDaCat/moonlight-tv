#include "coverloader.h"
#include "app.h"
#include "appitem.view.h"

#include <math.h>
#include <stddef.h>

#include "util/bus.h"
#include "util/path.h"

#include "libgamestream/client.h"
#include "libgamestream/errors.h"

#include <SDL_image.h>
#include "draw/sdl/lv_draw_sdl.h"
#include "misc/lv_lru.h"
#include "util/img_loader.h"
#include "refcounter.h"

#include "res.h"
#include "ui/tv_style.h"

typedef struct memcache_key_t {
    int id;
    uuidstr_t server_id;
    lv_coord_t target_width, target_height;
    bool backdrop;
} memcache_key_t;

typedef struct memcache_item_t {
    lv_img_dsc_t src;
    lv_sdl_img_data_t data;
    lv_ll_t objs;
} memcache_item_t;

typedef struct img_loader_req_t {
    coverloader_t *loader;
    uuidstr_t server_id;
    int id;
    lv_obj_t *target;
    lv_coord_t target_width, target_height;
    bool backdrop;
    /* Poster corner radius, baked into the image's alpha. */
    lv_coord_t radius;
    memcache_item_t *src;
    bool finished;
    SDL_Surface *cached;
    img_loader_task_t *task;
    struct img_loader_req_t *prev;
    struct img_loader_req_t *next;
} coverloader_req_t;

#define LINKEDLIST_IMPL
#define LINKEDLIST_TYPE coverloader_req_t
#define LINKEDLIST_PREFIX reqlist
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"
#include "lazy.h"
#include "draw/sdl/lv_draw_sdl_img.h"
#include "logging.h"

#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX
#undef LINKEDLIST_DOUBLE
#define LINKEDLIST_IMPL

#ifndef DEBUG
#define DEBUG 0
#endif

static const char *coverloader_cache_dir(coverloader_t *loader);

static GS_CLIENT coverloader_gs_client(coverloader_t *loader);

static void coverloader_cache_item_path(char path[4096], const coverloader_req_t *req);

static bool coverloader_memcache_get(coverloader_req_t *req);

static void coverloader_memcache_put(coverloader_req_t *req);

static bool coverloader_filecache_get(coverloader_req_t *req);

static void coverloader_filecache_put(coverloader_req_t *req);

static bool coverloader_fetch(coverloader_req_t *req);

static void coverloader_run_on_main(img_loader_t *loader, img_loader_run_on_main_fn fn, void *args);

static void img_loader_start_cb(coverloader_req_t *req);

static void img_loader_result_cb(coverloader_req_t *req);

static void img_loader_cancel_cb(coverloader_req_t *req);

/**
 *
 * @param obj lv_img instance
 * @param src loaded image, NULL will fallback to default cover
 */
static void img_set_cover(lv_obj_t *obj, memcache_item_t *src);

static struct memcache_item_t *memcache_item_new();

static void memcache_item_free(memcache_item_t *item);

static void coverloader_req_free(coverloader_req_t *req);

static int reqlist_find_by_target(coverloader_req_t *p, const void *v);

static bool cover_is_placeholder(const SDL_Surface *surface);
static void display_cover(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                          lv_coord_t width, lv_coord_t height, bool backdrop);
static SDL_Surface *make_backdrop(SDL_Surface *source, int width, int height);
static SDL_Surface *make_poster(SDL_Surface *source, int width, int height, int radius);

static void target_deleted_cb(lv_event_t *e);

static void target_src_unlink_cb(lv_event_t *e);

static void memcache_item_ref_obj(memcache_item_t *item, lv_obj_t *obj);

static void memcache_item_unref_obj(memcache_item_t *item, const lv_obj_t *obj);

static void purge_img_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item);

static const img_loader_impl_t coverloader_impl = {
        .memcache_get = coverloader_memcache_get,
        .memcache_put = coverloader_memcache_put,
        .filecache_get = coverloader_filecache_get,
        .filecache_put = coverloader_filecache_put,
        .fetch = coverloader_fetch,
        .run_on_main = coverloader_run_on_main,
};

static const img_loader_cb_t coverloader_cb = {
        .start_cb = img_loader_start_cb,
        .complete_cb = img_loader_result_cb,
        .fail_cb = img_loader_result_cb,
        .cancel_cb = img_loader_cancel_cb,
};

struct coverloader_t {
    img_loader_t *base_loader;
    lv_lru_t *mem_cache;
    lazy_t client;
    lazy_t cache_dir;
    coverloader_req_t *reqlist;
    refcounter_t refcounter;
};

typedef struct subimage_info_t {
    int w, h;
    SDL_Rect rect;
} subimage_info_t;

coverloader_t *coverloader_new(app_t *app) {
    coverloader_t *loader = malloc(sizeof(coverloader_t));
    refcounter_init(&loader->refcounter);
    loader->mem_cache = lv_lru_create(1024 * 1024 * 32, 720 * 1024, (lv_lru_free_t *) memcache_item_free, NULL);
    loader->base_loader = img_loader_create(&coverloader_impl, app->backend.executor);
    lazy_init(&loader->client, (lazy_supplier) app_gs_client_new, app);
    lazy_init(&loader->cache_dir, (lazy_supplier) path_cache, NULL);
    loader->reqlist = NULL;
    return loader;
}

void coverloader_unref(coverloader_t *loader) {
    if (!refcounter_unref(&loader->refcounter)) {
        return;
    }
    GS_CLIENT client = lazy_deinit(&loader->client);
    if (client != NULL) {
        gs_destroy(client);
    }
    char *cache_dir = lazy_deinit(&loader->cache_dir);
    if (cache_dir != NULL) {
        free(cache_dir);
    }
    img_loader_destroy(loader->base_loader);
    lv_lru_del(loader->mem_cache);
    refcounter_destroy(&loader->refcounter);
    free(loader);
}

void coverloader_display(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                         lv_coord_t target_width, lv_coord_t target_height) {
    display_cover(loader, uuid, id, target, target_width, target_height, false);
}

void coverloader_display_backdrop(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                                  lv_coord_t target_width, lv_coord_t target_height) {
    display_cover(loader, uuid, id, target, target_width, target_height, true);
}

static void display_cover(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                          lv_coord_t target_width, lv_coord_t target_height, bool backdrop) {
    if (target_width <= 0 || target_height <= 0) return;
    coverloader_req_t *existing = reqlist_find_by(loader->reqlist, target, reqlist_find_by_target);
    if (existing && existing->task) {
        img_loader_cancel(loader->base_loader, existing->task);
    }

    coverloader_req_t *req = reqlist_new();
    req->loader = loader;
    req->server_id = *uuid;
    req->id = id;
    req->target = target;
    req->target_width = target_width;
    req->target_height = target_height;
    req->backdrop = backdrop;
    req->radius = backdrop ? 0 : lv_obj_get_style_radius(target, LV_PART_MAIN);
    req->finished = false;
    lv_obj_add_event_cb(target, target_deleted_cb, LV_EVENT_DELETE, req);
    loader->reqlist = reqlist_append(loader->reqlist, req);
    refcounter_ref(&loader->refcounter);
    img_loader_task_t *task = img_loader_load(loader->base_loader, req, &coverloader_cb);
    /* If no task returned, then the request has been freed already */
    if (!task) { return; }
    req->task = task;
}

static const char *coverloader_cache_dir(coverloader_t *loader) {
    return lazy_obtain(&loader->cache_dir);
}

static GS_CLIENT coverloader_gs_client(coverloader_t *loader) {
    return lazy_obtain(&loader->client);
}

static void coverloader_cache_item_path(char path[4096], const coverloader_req_t *req) {
    const char *cachedir = coverloader_cache_dir(req->loader);
    char basename[128];
    SDL_snprintf(basename, 128, "%s_%d", (char *) &req->server_id, req->id);
    path_join_to(path, 4096, cachedir, basename);
}

static bool coverloader_memcache_get(coverloader_req_t *req) {
    // Uses result cache instead
    memcache_item_t *result = NULL;
    memcache_key_t key;
    memset(&key, 0, sizeof(key));
    key.id = req->id;
    key.server_id = req->server_id;
    key.target_width = req->target_width;
    key.target_height = req->target_height;
    key.backdrop = req->backdrop;

    (void) key.target_width;
    (void) key.target_height;

    lv_lru_get(req->loader->mem_cache, &key, sizeof(key), (void **) &result);
    req->src = result;

    return result != NULL;
}

static void coverloader_memcache_put(coverloader_req_t *req) {
    SDL_Surface *cached = req->cached;
    if (cached == NULL) {
        return;
    }
    memcache_item_t *result = NULL;
    memcache_key_t key;
    memset(&key, 0, sizeof(key));
    key.id = req->id;
    key.server_id = req->server_id;
    key.target_width = req->target_width;
    key.target_height = req->target_height;
    key.backdrop = req->backdrop;
    lv_lru_get(req->loader->mem_cache, &key, sizeof(key), (void **) &result);
    if (result == NULL) {
        lv_draw_sdl_drv_param_t *param = lv_disp_get_default()->driver->user_data;
        SDL_Renderer *renderer = param->renderer;

        result = memcache_item_new();
        lv_img_dsc_t *src = &result->src;
        lv_sdl_img_data_t *data = &result->data;
        data->type = LV_SDL_IMG_TYPE_TEXTURE;
        src->header.cf = LV_IMG_CF_TRUE_COLOR;
        if (SDL_ISPIXELFORMAT_ALPHA(cached->format->format)) {
            src->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        }
        size_t value_length = (size_t) cached->w * cached->h * sizeof(Uint32);
        if (cached->userdata) {
            subimage_info_t *info = cached->userdata;
            data->rect = info->rect;
            src->header.w = info->w;
            src->header.h = info->h;
        } else {
            src->header.w = cached->w;
            src->header.h = cached->h;
        }
        data->data.texture = SDL_CreateTextureFromSurface(renderer, cached);
        src->data_size = sizeof(lv_sdl_img_data_t);
        src->data = (const uint8_t *) data;
        lv_lru_set(req->loader->mem_cache, &key, sizeof(key), result, value_length);
    }
    req->src = result;
    req->finished = true;
    if (cached->userdata) {
        SDL_free(cached->userdata);
    }
    SDL_FreeSurface(cached);
}

static bool coverloader_filecache_get(coverloader_req_t *req) {
#if !DEBUG
    SDL_version ver;
    SDL_GetVersion(&ver);
    if (SDL_VERSIONNUM(ver.major, ver.minor, ver.patch) <= SDL_VERSIONNUM(2, 0, 1)) {
        return false;
    }
#endif
    char path[4096];
    coverloader_cache_item_path(path, req);
    SDL_Surface *decoded = IMG_Load(path);
    if (!decoded) {
        commons_log_warn("CoverLoader", "Failed to load cover from %s: %s", path, IMG_GetError());
        return false;
    }
    if (cover_is_placeholder(decoded)) {
        SDL_FreeSurface(decoded);
        return false;
    }
    if (req->backdrop) {
        req->cached = make_backdrop(decoded, req->target_width, req->target_height);
    } else {
        req->cached = make_poster(decoded, req->target_width, req->target_height, req->radius);
    }
    SDL_FreeSurface(decoded);
    return req->cached != NULL;
}

static bool cover_is_placeholder(const SDL_Surface *surface) {
    return (surface->w == 130 && surface->h == 180) || (surface->w == 628 && surface->h == 888);
}

/* Covers are prepared once on the image-loader worker at their display size, never per frame. */

/* Premultiplied color, 0-255 per channel. */
typedef struct pixelf_t {
    float r, g, b, a;
} pixelf_t;

static pixelf_t hex_pixel(uint32_t hex) {
    return (pixelf_t) {(float) ((hex >> 16) & 0xFF), (float) ((hex >> 8) & 0xFF), (float) (hex & 0xFF), 1};
}

static inline void pixelf_add(pixelf_t *sum, const pixelf_t *p, float k) {
    sum->r += k * p->r;
    sum->g += k * p->g;
    sum->b += k * p->b;
    sum->a += k * p->a;
}

static Uint32 pack_rgb(float r, float g, float b) {
    return (Uint32) LV_CLAMP(0, (int) (r + 0.5f), 255) << 16 | (Uint32) LV_CLAMP(0, (int) (g + 0.5f), 255) << 8 |
           (Uint32) LV_CLAMP(0, (int) (b + 0.5f), 255);
}

/* Taps of a box filter one output pixel wide, and at least one source pixel wide:
 * shrinking averages the covered area, enlarging interpolates linearly. */
typedef struct resample_axis_t {
    int taps;
    int *first;
    float *weights;
} resample_axis_t;

static void resample_axis_deinit(resample_axis_t *axis) {
    free(axis->first);
    free(axis->weights);
}

/* Spreads the source span [start, start + span) over len output pixels. */
static bool resample_axis_init(resample_axis_t *axis, int src_len, double start, double span, int len) {
    double scale = span / len, half = LV_MAX(scale, 1.0) / 2;
    axis->taps = LV_MIN((int) ceil(half * 2) + 1, src_len);
    axis->first = malloc(sizeof(int) * len);
    axis->weights = malloc(sizeof(float) * len * axis->taps);
    if (!axis->first || !axis->weights) {
        resample_axis_deinit(axis);
        return false;
    }
    for (int i = 0; i < len; ++i) {
        double center = start + (i + 0.5) * scale, lo = center - half, hi = center + half;
        int first = LV_CLAMP(0, (int) floor(lo), src_len - axis->taps);
        float *weights = &axis->weights[i * axis->taps];
        float total = 0;
        for (int t = 0; t < axis->taps; ++t) {
            double overlap = LV_MIN(hi, first + t + 1.0) - LV_MAX(lo, (double) (first + t));
            weights[t] = (float) LV_MAX(overlap, 0.0);
            total += weights[t];
        }
        for (int t = 0; t < axis->taps; ++t) {
            weights[t] /= total;
        }
        axis->first[i] = first;
    }
    return true;
}

/* A source rectangle of an ARGB8888 surface, resampled to width x height premultiplied pixels. */
static pixelf_t *resample(const SDL_Surface *argb, double x, double y, double w, double h, int width, int height) {
    resample_axis_t ax, ay;
    if (!resample_axis_init(&ax, argb->w, x, w, width)) return NULL;
    if (!resample_axis_init(&ay, argb->h, y, h, height)) {
        resample_axis_deinit(&ax);
        return NULL;
    }
    int row0 = ay.first[0], rows = ay.first[height - 1] + ay.taps - row0;
    pixelf_t *columns = malloc(sizeof(pixelf_t) * width * rows);
    pixelf_t *result = malloc(sizeof(pixelf_t) * width * height);
    if (columns && result) {
        for (int row = 0; row < rows; ++row) {
            const Uint32 *src = (const Uint32 *) ((const Uint8 *) argb->pixels + (row0 + row) * argb->pitch);
            pixelf_t *dst = &columns[row * width];
            for (int i = 0; i < width; ++i) {
                const Uint32 *px = &src[ax.first[i]];
                const float *weights = &ax.weights[i * ax.taps];
                pixelf_t sum = {0, 0, 0, 0};
                for (int t = 0; t < ax.taps; ++t) {
                    float wa = weights[t] * (float) (px[t] >> 24) / 255;
                    sum.r += wa * (float) ((px[t] >> 16) & 0xFF);
                    sum.g += wa * (float) ((px[t] >> 8) & 0xFF);
                    sum.b += wa * (float) (px[t] & 0xFF);
                    sum.a += wa;
                }
                dst[i] = sum;
            }
        }
        for (int i = 0; i < height; ++i) {
            const pixelf_t *src = &columns[(ay.first[i] - row0) * width];
            const float *weights = &ay.weights[i * ay.taps];
            pixelf_t *dst = &result[i * width];
            for (int col = 0; col < width; ++col) {
                pixelf_t sum = {0, 0, 0, 0};
                for (int t = 0; t < ay.taps; ++t) {
                    pixelf_add(&sum, &src[t * width + col], weights[t]);
                }
                dst[col] = sum;
            }
        }
    } else {
        free(result);
        result = NULL;
    }
    free(columns);
    resample_axis_deinit(&ax);
    resample_axis_deinit(&ay);
    return result;
}

/* Anti-aliased rounded corners in alpha, so drawing needs no clip mask. */
static void round_corners(SDL_Surface *surface, int radius) {
    radius = LV_MIN(radius, LV_MIN(surface->w, surface->h) / 2);
    for (int y = 0; y < radius; ++y) {
        Uint32 *top = (Uint32 *) ((Uint8 *) surface->pixels + y * surface->pitch);
        Uint32 *bottom = (Uint32 *) ((Uint8 *) surface->pixels + (surface->h - 1 - y) * surface->pitch);
        for (int x = 0; x < radius; ++x) {
            float dx = (float) radius - ((float) x + 0.5f), dy = (float) radius - ((float) y + 0.5f);
            float coverage = (float) radius + 0.5f - sqrtf(dx * dx + dy * dy);
            /* Coverage only grows toward the middle of the row. */
            if (coverage >= 1) break;
            Uint32 alpha = coverage > 0 ? (Uint32) (coverage * 255 + 0.5f) << 24 : 0;
            int mirror = surface->w - 1 - x;
            top[x] = (top[x] & 0xFFFFFF) | alpha;
            top[mirror] = (top[mirror] & 0xFFFFFF) | alpha;
            bottom[x] = (bottom[x] & 0xFFFFFF) | alpha;
            bottom[mirror] = (bottom[mirror] & 0xFFFFFF) | alpha;
        }
    }
}

/* The art center-cropped to the poster's shape, scaled to its exact size, corners rounded. */
static SDL_Surface *make_poster(SDL_Surface *source, int width, int height, int radius) {
    SDL_Surface *argb = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!argb) return NULL;
    double crop_w = argb->w, crop_h = argb->h;
    if (crop_w * height > crop_h * width) {
        crop_w = crop_h * width / height;
    } else {
        crop_h = crop_w * height / width;
    }
    pixelf_t *pixels = resample(argb, (argb->w - crop_w) / 2, (argb->h - crop_h) / 2, crop_w, crop_h,
                                width, height);
    SDL_FreeSurface(argb);
    if (!pixels) return NULL;
    SDL_Surface *result = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (result) {
        /* Transparent art sits on the placeholder's surface color. */
        const pixelf_t bg = hex_pixel(TV_SURFACE);
        for (int y = 0; y < height; ++y) {
            Uint32 *row = (Uint32 *) ((Uint8 *) result->pixels + y * result->pitch);
            const pixelf_t *src = &pixels[y * width];
            for (int x = 0; x < width; ++x) {
                float keep = 1 - src[x].a;
                row[x] = 0xFF000000u | pack_rgb(src[x].r + bg.r * keep, src[x].g + bg.g * keep, src[x].b + bg.b * keep);
            }
        }
        round_corners(result, radius);
    }
    free(pixels);
    return result;
}

/* Built small and stretched by the GPU: after this much blur, more pixels add nothing. */
#define BACKDROP_SCALE 8
#define BACKDROP_SATURATION 1.8f
#define BACKDROP_BRIGHTNESS 0.9f

typedef struct shade_stop_t {
    float at, alpha;
} shade_stop_t;

/* Background-colored gradients over the art: from the top, from the bottom behind the shelf,
 * and from the left behind the title. */
static const shade_stop_t shade_top[] = {{0, .75f}, {.16f, 0}};
static const shade_stop_t shade_bottom[] = {{0, .97f}, {.32f, .8f}, {.62f, 0}};
static const shade_stop_t shade_left[] = {{0, .94f}, {.36f, .7f}, {.72f, .2f}, {1, .1f}};

static float shade_alpha(const shade_stop_t *stops, int count, float at) {
    if (at <= stops[0].at) return stops[0].alpha;
    for (int i = 1; i < count; ++i) {
        if (at <= stops[i].at) {
            float f = (at - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
            return stops[i - 1].alpha + (stops[i].alpha - stops[i - 1].alpha) * f;
        }
    }
    return stops[count - 1].alpha;
}

/* One box pass along each line, transparent past the ends. */
static void blur_lines(const pixelf_t *src, pixelf_t *dst, int len, int lines, int step, int line_step, int radius) {
    float norm = 1.0f / (float) (2 * radius + 1);
    for (int line = 0; line < lines; ++line) {
        const pixelf_t *in = &src[line * line_step];
        pixelf_t *out = &dst[line * line_step];
        pixelf_t sum = {0, 0, 0, 0};
        for (int i = 0; i < LV_MIN(radius, len); ++i) {
            pixelf_add(&sum, &in[i * step], 1);
        }
        for (int i = 0; i < len; ++i) {
            if (i + radius < len) pixelf_add(&sum, &in[(i + radius) * step], 1);
            out[i * step] = (pixelf_t) {sum.r * norm, sum.g * norm, sum.b * norm, sum.a * norm};
            if (i >= radius) pixelf_add(&sum, &in[(i - radius) * step], -1);
        }
    }
}

/* The hero's full-screen glow, as the mockup's CSS draws it: the art covers a box from 30% across to past
 * the right edge, overhanging top and bottom ("center 30% / cover"), then blur(1/16 width), saturate(1.8),
 * brightness(.9), over the background, under the shades. */
static SDL_Surface *make_backdrop(SDL_Surface *source, int width, int height) {
    int w = LV_MAX(width / BACKDROP_SCALE, 1), h = LV_MAX(height / BACKDROP_SCALE, 1);
    /* Three box passes of this radius approximate a Gaussian of the same deviation. */
    int radius = LV_MAX(w / 16, 1), margin = 3 * radius;
    int canvas_w = w + 2 * margin, canvas_h = h + 2 * margin;
    SDL_Surface *argb = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!argb) return NULL;
    double box_x = w * 0.3, box_y = h * -0.15, box_w = w * 0.8, box_h = h * 1.3;
    double scale = LV_MAX(box_w / argb->w, box_h / argb->h);
    double art_x = box_x + (box_w - argb->w * scale) / 2, art_y = box_y + (box_h - argb->h * scale) * 0.3;
    int x0 = (int) lround(margin + box_x), x1 = (int) lround(margin + box_x + box_w);
    int y0 = (int) lround(margin + box_y), y1 = (int) lround(margin + box_y + box_h);
    pixelf_t *art = resample(argb, (x0 - margin - art_x) / scale, (y0 - margin - art_y) / scale,
                             (x1 - x0) / scale, (y1 - y0) / scale, x1 - x0, y1 - y0);
    SDL_FreeSurface(argb);
    pixelf_t *canvas = calloc((size_t) canvas_w * canvas_h, sizeof(pixelf_t));
    pixelf_t *scratch = malloc(sizeof(pixelf_t) * canvas_w * canvas_h);
    subimage_info_t *info = SDL_malloc(sizeof(subimage_info_t));
    SDL_Surface *result = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGB888);
    if (art && canvas && scratch && info && result) {
        for (int y = y0; y < y1; ++y) {
            memcpy(&canvas[y * canvas_w + x0], &art[(y - y0) * (x1 - x0)], sizeof(pixelf_t) * (x1 - x0));
        }
        for (int pass = 0; pass < 3; ++pass) {
            blur_lines(canvas, scratch, canvas_w, canvas_h, 1, canvas_w, radius);
            blur_lines(scratch, canvas, canvas_h, canvas_w, canvas_w, 1, radius);
        }
        const pixelf_t bg = hex_pixel(TV_BACKGROUND);
        const float s = BACKDROP_SATURATION;
        for (int y = 0; y < h; ++y) {
            float at = ((float) y + 0.5f) / (float) h;
            float keep_y = (1 - shade_alpha(shade_top, 2, at)) * (1 - shade_alpha(shade_bottom, 3, 1 - at));
            const pixelf_t *src = &canvas[(y + margin) * canvas_w + margin];
            Uint32 *row = (Uint32 *) ((Uint8 *) result->pixels + y * result->pitch);
            for (int x = 0; x < w; ++x) {
                const pixelf_t *p = &src[x];
                pixelf_t c = bg;
                if (p->a > 0) {
                    /* Graded on the art's own color, then laid over the background. */
                    float r = p->r / p->a, g = p->g / p->a, b = p->b / p->a;
                    float sr = (0.213f + 0.787f * s) * r + (0.715f - 0.715f * s) * g + (0.072f - 0.072f * s) * b;
                    float sg = (0.213f - 0.213f * s) * r + (0.715f + 0.285f * s) * g + (0.072f - 0.072f * s) * b;
                    float sb = (0.213f - 0.213f * s) * r + (0.715f - 0.715f * s) * g + (0.072f + 0.928f * s) * b;
                    float k = BACKDROP_BRIGHTNESS * p->a;
                    c.r = LV_CLAMP(0.0f, sr, 255.0f) * k + bg.r * (1 - p->a);
                    c.g = LV_CLAMP(0.0f, sg, 255.0f) * k + bg.g * (1 - p->a);
                    c.b = LV_CLAMP(0.0f, sb, 255.0f) * k + bg.b * (1 - p->a);
                }
                float keep = keep_y * (1 - shade_alpha(shade_left, 4, ((float) x + 0.5f) / (float) w));
                row[x] = pack_rgb(bg.r + (c.r - bg.r) * keep, bg.g + (c.g - bg.g) * keep, bg.b + (c.b - bg.b) * keep);
            }
        }
        /* Stretched over the whole target. */
        info->w = width;
        info->h = height;
        info->rect = (SDL_Rect) {0, 0, 0, 0};
        result->userdata = info;
        info = NULL;
    } else if (result) {
        SDL_FreeSurface(result);
        result = NULL;
    }
    SDL_free(info);
    free(scratch);
    free(canvas);
    free(art);
    return result;
}

static void coverloader_filecache_put(coverloader_req_t *req) {
    // This was done in fetch step
    (void) req;
}

static bool coverloader_fetch(coverloader_req_t *req) {
#if !DEBUG
    SDL_version ver;
    SDL_GetVersion(&ver);
    if (SDL_VERSIONNUM(ver.major, ver.minor, ver.patch) <= SDL_VERSIONNUM(2, 0, 1)) {
        return NULL;
    }
#endif
    const pclist_t *node = pcmanager_node(pcmanager, &req->server_id);
    if (!node) {
        return false;
    }
    char path[4096];
    coverloader_cache_item_path(path, req);
    GS_CLIENT client = coverloader_gs_client(req->loader);
    if (gs_download_cover(client, node->server, req->id, path) != GS_OK) {
        return false;
    }
    return coverloader_filecache_get(req);
}

static void coverloader_run_on_main(img_loader_t *loader, img_loader_run_on_main_fn fn, void *args) {
    (void) loader;
    app_bus_post(global, (bus_actionfunc) fn, args);
}

static void img_loader_start_cb(coverloader_req_t *req) {
    /* A backdrop keeps showing the previous art until the next one is ready. */
    if (!req->backdrop) img_set_cover(req->target, NULL);
}

static void img_loader_result_cb(coverloader_req_t *req) {
    req->task = NULL;
    coverloader_t *loader = req->loader;
    if (req->target == NULL) {
        goto done;
    }

    lv_obj_remove_event_cb(req->target, target_deleted_cb);
    img_set_cover(req->target, req->src);
    done:
    loader->reqlist = reqlist_remove(loader->reqlist, req);
    coverloader_req_free(req);
    coverloader_unref(loader);
}

static void img_loader_cancel_cb(coverloader_req_t *req) {
    if (req->target) {
        lv_obj_remove_event_cb(req->target, target_deleted_cb);
    }
    req->task = NULL;
    coverloader_t *loader = req->loader;
    loader->reqlist = reqlist_remove(loader->reqlist, req);
    coverloader_req_free(req);
    coverloader_unref(loader);
}

static void img_set_cover(lv_obj_t *obj, memcache_item_t *src) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(obj);

    /* If the old src is not default cover, assume it's memcache item and unref this obj from it */
    const void *old_src = lv_img_get_src(obj);
    bool skip_add_del_cb = false;
    if (old_src != NULL && lv_img_src_get_type(old_src) == LV_IMG_SRC_VARIABLE) {
        const memcache_item_t *old_img = old_src;
        /* for memcache item, src.data is pointer to data */
        if (old_img->src.data == (const void *) &old_img->data && old_img->src.data_size == sizeof(lv_sdl_img_data_t)) {
            memcache_item_unref_obj((memcache_item_t *) old_img, obj);
            skip_add_del_cb = true;
        }
    }

    if (src != NULL) {
        lv_img_set_src(obj, src);
        memcache_item_ref_obj(src, obj);
        // If the object is deleted, mark the src orphaned. So it will not access obj when recycled.
        if (!skip_add_del_cb) {
            lv_obj_add_event_cb(obj, target_src_unlink_cb, LV_EVENT_DELETE, NULL);
        }
        /* Loaded: the art shows instead of the placeholder. */
        lv_obj_add_state(obj, LV_STATE_USER_1);
    } else {
        lv_img_set_src(obj, &holder->styles->defcover_src);
        lv_obj_remove_event_cb(obj, target_src_unlink_cb);
        lv_obj_clear_state(obj, LV_STATE_USER_1);
    }
}

struct memcache_item_t *memcache_item_new() {
    memcache_item_t *item = calloc(1, sizeof(memcache_item_t));
    _lv_ll_init(&item->objs, sizeof(lv_obj_t *));
    return item;
}

static void memcache_item_free(memcache_item_t *item) {
    lv_disp_drv_t *driver = lv_disp_get_default()->driver;
    lv_draw_sdl_ctx_t *ctx = (lv_draw_sdl_ctx_t *) driver->draw_ctx;

    /* Make sure the obj isn't referencing the src anymore */
    lv_obj_t **i;
    _LV_LL_READ(&item->objs, i) {
        lv_obj_t *obj = *i;
        if (obj == NULL) { continue; }
        appitem_viewholder_t *holder = lv_obj_get_user_data(obj);
        lv_img_set_src(obj, &holder->styles->defcover_src);
        lv_obj_remove_event_cb(obj, target_src_unlink_cb);
        lv_obj_clear_state(obj, LV_STATE_USER_1);
    }
    SDL_DestroyTexture(item->data.data.texture);

    // Purge internal texture cache too
    purge_img_cache(ctx, item);

    /* unref all objs to this item */
    _lv_ll_clear(&item->objs);
    free(item);
}

static void purge_img_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item) {
    struct __attribute__ ((__packed__)) {
        lv_draw_sdl_cache_key_head_img_t header;
        const void *src;
    } key_to_remove;
    memset(&key_to_remove, 0, sizeof(key_to_remove));
    key_to_remove.header.magic = LV_GPU_CACHE_KEY_MAGIC_IMG;
    key_to_remove.header.type = LV_IMG_SRC_VARIABLE;
    key_to_remove.header.frame_id = 0;
    key_to_remove.src = &item->src;

    lv_lru_remove(ctx->internals->texture_cache, &key_to_remove, sizeof(key_to_remove));
}

static void memcache_item_ref_obj(memcache_item_t *item, lv_obj_t *obj) {
    lv_obj_t **node = _lv_ll_ins_tail(&item->objs);
    *node = obj;
}

static void memcache_item_unref_obj(memcache_item_t *item, const lv_obj_t *obj) {
    lv_obj_t **i;
    _LV_LL_READ_BACK(&item->objs, i) {
        if (obj == *i) {
            break;
        }
    }
    if (i == NULL) { return; }
    _lv_ll_remove(&item->objs, i);
    lv_mem_free(i);
}

static inline void coverloader_req_free(coverloader_req_t *req) {
    free(req);
}

static int reqlist_find_by_target(coverloader_req_t *p, const void *v) {
    return p->target != v;
}

static void target_deleted_cb(lv_event_t *e) {
    coverloader_req_t *req = lv_event_get_user_data(e);
    req->target = NULL;
}

static void target_src_unlink_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    memcache_item_unref_obj((memcache_item_t *) lv_img_get_src(obj), obj);
}
