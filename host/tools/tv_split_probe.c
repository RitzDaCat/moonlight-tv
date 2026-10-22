// SPDX-License-Identifier: GPL-3.0-only
/* Exercise two real SMP video instances without a game, network stream, or
 * changes to Moonlight preferences. All SDL window operations stay on main. */
#include <SDL.h>
#include <ss4s/modapi.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Frame { unsigned char *data; uint32_t size; } Frame;
typedef struct Side {
    SS4S_PlayerContext *context;
    SS4S_VideoInstance *video;
    Frame *frames;
    uint32_t width, height, fps, count, next;
    unsigned fed, refused;
    atomic_uint completed;
    uint64_t due;
} Side;
static volatile sig_atomic_t stopping;
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}
static void stop(int sig) { (void)sig; stopping = 1; }
static void log_message(SS4S_LogLevel level, const char *tag, const char *format, ...) {
    va_list args;
    fprintf(stdout, "[%d:%s] ", level, tag);
    va_start(args, format); vfprintf(stdout, format, args); va_end(args);
    fputc('\n', stdout); fflush(stdout);
}
static uint32_t begin_frame(SS4S_Player *player) { (void)player; return 0; }
static void end_frame(SS4S_Player *player, uint32_t token) { (void)player; (void)token; }
static void report_frame(SS4S_Player *player, uint32_t latency) {
    (void)latency;
    atomic_fetch_add(&((Side *)player)->completed, 1);
}
static uint32_t read_u32(FILE *file) {
    unsigned char b[4];
    if (fread(b, 1, sizeof(b), file) != sizeof(b)) return 0;
    return (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}
static bool read_fixture(Side *side, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool valid = false;
    if (read_u32(file) != 0x50534c4d) goto done;
    side->width = read_u32(file); side->height = read_u32(file);
    side->fps = read_u32(file); side->count = read_u32(file);
    if (!side->width || side->width > 1920 || !side->height || side->height > 1080 ||
        !side->fps || side->fps > 120 || !side->count || side->count > 120) goto done;
    side->frames = calloc(side->count, sizeof(Frame));
    if (!side->frames) goto done;
    for (unsigned i=0; i<side->count; ++i) {
        Frame *frame = &side->frames[i]; frame->size = read_u32(file);
        if (!frame->size || frame->size > 2*1024*1024) goto done;
        frame->data = malloc(frame->size);
        if (!frame->data || fread(frame->data, 1, frame->size, file) != frame->size) goto done;
    }
    valid = true;
done:
    fclose(file); return valid;
}
static bool open_side(Side *side, int index, const SS4S_Module *module, int width, int height) {
    side->context = module->PlayerDriver->Create((SS4S_Player *)side);
    if (!side->context) return false;
    SS4S_VideoInfo info = {.codec=SS4S_VIDEO_H264, .width=side->width, .height=side->height,
                          .frameRateNumerator=side->fps, .frameRateDenominator=1};
    SS4S_VideoExtraInfo extra = {.viewportWidth=width, .viewportHeight=height};
    if (module->VideoDriver->Open(&info, &extra, &side->video, side->context) != SS4S_VIDEO_OPEN_OK) return false;
    int half=width/2, h=half*(int)side->height/(int)side->width;
    SS4S_VideoRect area = {.x=index*half, .y=(height-h)/2, .width=half, .height=h};
    if (!module->VideoDriver->SetDisplayArea(side->video, NULL, &area)) return false;
    side->due = now_ms();
    printf("side_open=%d size=%ux%u fps=%u\n", index, side->width, side->height, side->fps);
    return true;
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char dir[PATH_MAX], path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", dir, sizeof(dir)-1);
    if (len <= 0) return 2;
    dir[len]=0; char *slash=strrchr(dir, '/'); if (!slash) return 2; *slash=0;
    if (snprintf(path, sizeof(path), "%s/probe.log", dir) >= (int)sizeof(path)) return 2;
    if (!freopen(path, "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, stop); signal(SIGINT, stop);
    Side sides[2] = {0};
    SS4S_Module module = {0};
    SDL_Window *window=NULL; SDL_Renderer *renderer=NULL;
    bool initialized=false; int result=1;
    const char *library = "/media/developer/apps/usr/palm/applications/com.limelight.webos/lib/ss4s-smp-webos.so";
    void *handle=dlopen(library, RTLD_NOW);
    if (!handle) { printf("module_error=%s\n", dlerror()); goto cleanup; }
    SS4S_ModuleOpenFunction *open_module=dlsym(handle, "SS4S_ModuleOpen_SMP_WEBOS");
    SS4S_LibraryContext lib={.Log=log_message, .VideoStats={begin_frame, end_frame, report_frame}};
    if (!open_module || !open_module(&module, &lib)) goto cleanup;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_error=%s\n", SDL_GetError()); goto cleanup; }
    initialized=true;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) != 0) goto cleanup;
    window=SDL_CreateWindow("Two-player decoder test", 0, 0, mode.w, mode.h, SDL_WINDOW_FULLSCREEN);
    if (!window) goto cleanup;
    renderer=SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) goto cleanup;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
    for (int i=0; i<2; ++i) {
        if (snprintf(path, sizeof(path), "%s/player%d.frames", dir, i+1) >= (int)sizeof(path) ||
            !read_fixture(&sides[i], path)) { puts("fixture_error"); goto cleanup; }
    }
    if (!open_side(&sides[0], 0, &module, mode.w, mode.h)) { puts("first_open_failed"); goto cleanup; }
    uint64_t start=now_ms(); unsigned before=0; bool second=false;
    while (!stopping && now_ms()-start < 14000) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) if (event.type==SDL_QUIT ||
            (event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE)) stopping=1;
        uint64_t now=now_ms();
        if (!second && now-start >= 3000) {
            before=atomic_load(&sides[0].completed);
            printf("before_second_first_completed=%u\n", before);
            if (!open_side(&sides[1], 1, &module, mode.w, mode.h)) { puts("second_open_failed"); break; }
            second=true;
        }
        for (int i=0; i<(second?2:1); ++i) {
            Side *side=&sides[i]; if (now<side->due) continue;
            Frame *frame=&side->frames[side->next];
            SS4S_VideoFeedResult feed=module.VideoDriver->Feed(side->video, frame->data, frame->size,
                    SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END);
            if (feed==SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
                /* Match Moonlight's recovery: resume at a known IDR, never
                 * continue dependent P frames after a rejected access unit. */
                side->refused++; side->next=0; side->due=now+1000/side->fps;
                continue;
            }
            if (feed==SS4S_VIDEO_FEED_ERROR) {
                printf("feed_error_side=%d code=%d\n", i, feed); stopping=1; break;
            }
            if (feed==SS4S_VIDEO_FEED_OK) {
                side->fed++; side->next=(side->next+1)%side->count;
                side->due=now+1000/side->fps;
            } else side->refused++;
        }
        SDL_Delay(1);
    }
    unsigned completed0=atomic_load(&sides[0].completed), completed1=atomic_load(&sides[1].completed);
    result = !stopping && second && before>0 && completed0>before+30 && completed1>30 ? 0:1;
    printf("RESULT first_before=%u first_after=%u second=%u fed0=%u fed1=%u refused0=%u refused1=%u overlap_callbacks=%s\n",
           before, completed0, completed1, sides[0].fed, sides[1].fed, sides[0].refused,
           sides[1].refused, result==0?"yes":"unproven");
cleanup:
    for (int i=1; i>=0; --i) {
        if (sides[i].video) module.VideoDriver->Close(sides[i].video);
        if (sides[i].context) module.PlayerDriver->Destroy(sides[i].context);
        if (sides[i].frames) for (unsigned n=0; n<sides[i].count; ++n) free(sides[i].frames[n].data);
        free(sides[i].frames);
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (initialized) SDL_Quit();
    if (handle) dlclose(handle);
    printf("probe_exit=%d\n", result);
    return result;
}
