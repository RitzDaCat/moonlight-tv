/* Native SDL -> LVGL focus coverage. Host/decoder I/O is deliberately replaced. */
#include "app.h"
#include "backend/pcmanager/priv.h"
#include "backend/pcmanager/pclist.h"
#include "backend/apploader/apploader.h"
#include "ui/launcher/launcher.controller.h"
#include "ui/launcher/apps.controller.h"
#include "ui/settings/settings.controller.h"
#include "ui/streaming/streaming.controller.h"
#include "lv_gridview.h"
#include "draw/sdl/lv_draw_sdl.h"
#include "util/bus.h"
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(2); } } while (0)

struct apploader_t { apploader_cb_t cb; void *userdata; };
static apploader_t *fixture_loader;

apploader_t *__wrap_apploader_create(app_t *app, const uuidstr_t *uuid, const apploader_cb_t *cb, void *userdata) {
    (void) app; (void) uuid;
    apploader_t *loader = calloc(1, sizeof(*loader));
    loader->cb = *cb;
    loader->userdata = userdata;
    fixture_loader = loader;
    return loader;
}
void __wrap_apploader_load(apploader_t *loader) { (void) loader; }
void __wrap_apploader_cancel(apploader_t *loader) { (void) loader; }
void __wrap_apploader_destroy(apploader_t *loader) {
    if (fixture_loader == loader) fixture_loader = NULL;
    free(loader);
}
apploader_state_t __wrap_apploader_state(apploader_t *loader) { (void) loader; return APPLOADER_STATE_IDLE; }
void __wrap_pcmanager_auto_discovery_start(pcmanager_t *manager) { (void) manager; }
void __wrap_pcmanager_auto_discovery_stop(pcmanager_t *manager) { (void) manager; }
void __wrap_pcmanager_request_update(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t cb, void *data) {
    (void) manager; (void) uuid; (void) cb; (void) data;
}
bool __wrap_app_is_decoder_valid(app_t *app) { (void) app; return true; }
int __wrap_app_session_begin(app_t *app, const uuidstr_t *uuid, const APP_LIST *gs_app,
                             const uuidstr_t *split_uuid, const APP_LIST *split_app) {
    (void) app; (void) uuid; (void) gs_app; (void) split_uuid; (void) split_app;
    return 0;
}
void __wrap_app_input_init_gamepad_mapping(app_input_t *input, executor_t *executor, const app_settings_t *settings) {
    (void) input; (void) executor; (void) settings;
}
void __wrap_app_input_deinit_gamepad_mapping(app_input_t *input) { (void) input; }
GS_CLIENT __wrap_app_gs_client_new(app_t *app) { (void) app; return NULL; }
int __wrap_gs_download_cover(GS_CLIENT client, PSERVER_DATA server, int id, const char *destination) {
    (void) client; (void) server; (void) id;
    char asset_key[32];
    snprintf(asset_key, sizeof(asset_key), "MOONLIGHT_UI_ART_%d", id);
    const char *source = getenv(asset_key);
    if (!source) source = getenv("MOONLIGHT_UI_ART");
    if (!source) return -1;
    FILE *in = fopen(source, "rb"), *out = fopen(destination, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }
    char buffer[8192]; size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), in))) CHECK(fwrite(buffer, 1, size, out) == size);
    fclose(in); fclose(out);
    return 0;
}
SDL_Renderer *__real_SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
SDL_Renderer *__wrap_SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags) {
    (void) flags;
    return __real_SDL_CreateRenderer(window, index, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
}

static int load_settings(app_settings_t *settings) {
    settings_initialize(settings, SDL_strdup(getenv("MOONLIGHT_UI_SCRATCH")));
    settings->conf_persistent = true;
    settings->fullscreen = false;
    settings->window_state.w = 1920;
    settings->window_state.h = 1080;
    return 0;
}

static void pump(app_t *app, int frames) {
    for (int i = 0; i < frames; ++i) {
        app_process_events(app);
        lv_timer_handler();
        SDL_Delay(8);
    }
}

static void button(app_t *app, Uint8 key) {
    SDL_Event event = {.cbutton = {.type=SDL_CONTROLLERBUTTONDOWN, .which=123, .button=key, .state=SDL_PRESSED}};
    CHECK(SDL_PushEvent(&event) == 1);
    pump(app, 5);
    event.cbutton.type = SDL_CONTROLLERBUTTONUP;
    event.cbutton.state = SDL_RELEASED;
    CHECK(SDL_PushEvent(&event) == 1);
    pump(app, 5);
}

static void capture(app_t *app, const char *name) {
    const char *directory = getenv("MOONLIGHT_UI_CAPTURE");
    if (!directory) return;
    lv_refr_now(app->ui.disp);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, app->ui.width, app->ui.height, 32, SDL_PIXELFORMAT_RGBA32);
    CHECK(surface);
    lv_draw_sdl_drv_param_t *param = app->ui.disp->driver->user_data;
    CHECK(SDL_RenderReadPixels(param->renderer, NULL, surface->format->format, surface->pixels, surface->pitch) == 0);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/%s.png", directory, name) < (int) sizeof(path));
    CHECK(IMG_SavePNG(surface, path) == 0);
    SDL_FreeSurface(surface);
}

static void provide_apps(void) {
    CHECK(fixture_loader);
    apploader_list_t *apps = calloc(1, sizeof(*apps));
    apps->count = 4;
    apps->items = calloc(apps->count, sizeof(*apps->items));
    const char *names[] = {"Monster Hunter", "HELLDIVERS 2", "Steam", "Desktop"};
    for (size_t i = 0; i < apps->count; ++i) {
        apps->items[i].base.id = (int) i + 1;
        apps->items[i].base.name = SDL_strdup(names[i]);
    }
    fixture_loader->cb.data(apps, fixture_loader->userdata);
}

static void test_streaming_overlay(app_t *app) {
    app->settings.split_audio_gain_db[0] = -12;
    app->settings.split_audio_gain_db[1] = -18;
    app->settings.split_audio_muted[0] = false;
    streaming_scene_arg_t args = {.global=app, .split=true};
    streaming_controller_t *overlay = (streaming_controller_t *) lv_fragment_create(&streaming_controller_class, &args);
    lv_fragment_create_obj(&overlay->base, lv_scr_act());
    lv_obj_clear_flag(overlay->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(overlay->stream_audio[0].gain);
    button(app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(app->settings.split_audio_gain_db[0] == -11);
    CHECK(lv_group_get_focused(overlay->group) == overlay->stream_audio[0].gain);
    button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(overlay->group) == overlay->stream_audio[0].muted);
    CHECK(lv_slider_get_value(overlay->stream_audio[0].gain) == -11);
    CHECK(app->settings.split_audio_gain_db[0] == -11);
    button(app, SDL_CONTROLLER_BUTTON_A);
    CHECK(app->settings.split_audio_muted[0]);
    CHECK(!lv_obj_has_flag(overlay->overlay, LV_OBJ_FLAG_HIDDEN));
    button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(overlay->group) == overlay->stream_audio[1].gain);
    CHECK(app->settings.split_audio_muted[0]);
    button(app, SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(lv_group_get_focused(overlay->group) == overlay->stream_audio[0].muted);
    CHECK(lv_slider_get_value(overlay->stream_audio[1].gain) == -18);
    CHECK(app->settings.split_audio_gain_db[1] == -18);
    lv_group_focus_obj(overlay->stats_pin);
    button(app, SDL_CONTROLLER_BUTTON_A);
    CHECK(lv_obj_has_state(overlay->stats_pin, LV_STATE_CHECKED));
    button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(overlay->group) == overlay->stream_audio[0].gain);
    CHECK(lv_obj_has_state(overlay->stats_pin, LV_STATE_CHECKED));
    lv_group_focus_obj(overlay->stats_pin);
    button(app, SDL_CONTROLLER_BUTTON_A);
    CHECK(!lv_obj_has_state(overlay->stats_pin, LV_STATE_CHECKED));
    capture(app, "stream-overlay");
    lv_group_focus_obj(overlay->kbd_btn);
    lv_obj_t *visited[9];
    CHECK(lv_group_get_obj_count(overlay->group) == 9);
    for (int i = 0; i < 9; ++i) {
        visited[i] = lv_group_get_focused(overlay->group);
        for (int j = 0; j < i; ++j) CHECK(visited[j] != visited[i]);
        button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    }
    CHECK(lv_group_get_focused(overlay->group) == overlay->kbd_btn);
    button(app, SDL_CONTROLLER_BUTTON_B);
    CHECK(lv_obj_has_flag(overlay->overlay, LV_OBJ_FLAG_HIDDEN));
    lv_fragment_del(&overlay->base);
}

int main(int argc, char **argv) {
    app_t app;
    CHECK(getenv("MOONLIGHT_UI_SCRATCH"));
    CHECK(app_init(&app, load_settings, argc, argv) == 0);
    uuidstr_t uuid;
    uuidstr_fromstr(&uuid, "11111111-1111-4111-8111-111111111111");
    SERVER_DATA *server = serverdata_new();
    server->uuid = SDL_strdup((const char *) &uuid);
    server->hostname = SDL_strdup("My PC");
    server->serverInfo.address = SDL_strdup("127.0.0.1");
    server->paired = true;
    pclist_t *host = pclist_insert_known(pcmanager, &uuid, server);
    host->state.code = SERVER_STATE_AVAILABLE;
    host->selected = true;
    app_ui_open(&app.ui, true, NULL);
    ui_set_input_mode(&app.ui.input, UI_INPUT_MODE_GAMEPAD);
    provide_apps();
    launcher_fragment_t *launcher = launcher_instance();
    CHECK(launcher);
    launcher_focus_home(launcher);
    pump(&app, 45);
    apps_fragment_t *apps = (apps_fragment_t *) lv_fragment_manager_find_by_container(launcher->base.child_manager, launcher->detail);
    CHECK(apps);
    CHECK(lv_group_get_focused(launcher->detail_group) == apps->launch_btn);
    capture(&app, "home");
    SDL_Event axis = {.caxis = {.type=SDL_CONTROLLERAXISMOTION, .which=123,
                               .axis=SDL_CONTROLLER_AXIS_LEFTX, .value=20000}};
    CHECK(SDL_PushEvent(&axis) == 1);
    pump(&app, 5);
    CHECK(lv_group_get_focused(launcher->detail_group) == apps->options_btn);
    axis.caxis.value = 0;
    CHECK(SDL_PushEvent(&axis) == 1);
    pump(&app, 5);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    button(&app, SDL_CONTROLLER_BUTTON_X);
    CHECK(app.ui.input.key.indev->group != launcher->detail_group);
    button(&app, SDL_CONTROLLER_BUTTON_B);
    CHECK(app.ui.input.key.indev->group == launcher->detail_group);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(lv_group_get_focused(launcher->detail_group) == apps->options_btn);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(launcher->detail_group) == apps->applist);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(apps->featured_id == 2);
    pump(&app, 10);
    capture(&app, "shelf");
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(lv_group_get_focused(launcher->detail_group) == apps->launch_btn);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(app_input_get_group(&app.ui.input) == launcher->nav_group);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(lv_group_get_focused(launcher->nav_group) == launcher->computers_btn);
    button(&app, SDL_CONTROLLER_BUTTON_A);
    CHECK(app_input_get_group(&app.ui.input) == launcher->host_group);
    CHECK(launcher->computers_opened);
    capture(&app, "computers");
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(launcher->host_group) == launcher->add_btn);
    button(&app, SDL_CONTROLLER_BUTTON_A);
    CHECK(app.ui.input.key.indev->group != launcher->host_group);
    button(&app, SDL_CONTROLLER_BUTTON_B);
    if (app.ui.input.key.indev->group != launcher->host_group) button(&app, SDL_CONTROLLER_BUTTON_B);
    CHECK(app.ui.input.key.indev->group == launcher->host_group);
    button(&app, SDL_CONTROLLER_BUTTON_B);
    CHECK(!launcher->computers_opened);
    CHECK(app_input_get_group(&app.ui.input) == launcher->nav_group);
    lv_group_focus_obj(launcher->pref_btn);
    button(&app, SDL_CONTROLLER_BUTTON_A);
    settings_controller_t *settings = (settings_controller_t *) lv_fragment_manager_get_top(app.ui.fm);
    CHECK(settings->base.cls == &settings_controller_cls);
    for (int i = 0; i < 6; ++i) {
        if (lv_group_get_obj_count(settings->detail_group)) {
            button(&app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            CHECK(app_input_get_group(&app.ui.input) == settings->detail_group);
            CHECK(lv_group_get_focused(settings->detail_group));
            button(&app, SDL_CONTROLLER_BUTTON_B);
            CHECK(app_input_get_group(&app.ui.input) == settings->nav_group);
        }
        if (i < 5) button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    }
    capture(&app, "settings");
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(settings->nav_group) == settings->close_btn);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(lv_group_get_focused(settings->nav_group) != settings->close_btn);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    button(&app, SDL_CONTROLLER_BUTTON_A);
    CHECK(lv_fragment_manager_get_top(app.ui.fm)->cls == &launcher_controller_class);
    launcher = launcher_instance();
    CHECK(lv_group_get_focused(launcher->nav_group) == launcher->pref_btn);
    button(&app, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    CHECK(lv_group_get_focused(launcher->nav_group) == launcher->help_btn);
    button(&app, SDL_CONTROLLER_BUTTON_A);
    lv_group_t *help_group = app.ui.input.key.indev->group;
    CHECK(help_group != launcher->nav_group);
    lv_obj_t *help_first = lv_group_get_focused(help_group);
    button(&app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(lv_group_get_focused(help_group) != help_first);
    button(&app, SDL_CONTROLLER_BUTTON_B);
    CHECK(app.ui.input.key.indev->group == launcher->nav_group);
    test_streaming_overlay(&app);
    launcher_focus_navigation(launcher);
    pump(&app, 4);
    app_deinit(&app);
    puts("PASS native controller home, shelf, computer/add dialogs, settings, Help, shoulders, stream audio and Back");
    return 0;
}
