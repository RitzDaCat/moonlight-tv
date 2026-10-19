#include <SDL.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
static int sleeps, waits;
static void record_sleep(Uint32 ms) { sleeps++; SDL_Delay(ms); }
static int record_wait(SDL_Event* event, int ms) { waits++; return SDL_WaitEventTimeout(event, ms); }
#define SDL_Delay record_sleep
#define SDL_WaitEventTimeout record_wait
#include "input/event_wait.h"
#undef SDL_Delay
#undef SDL_WaitEventTimeout

int main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS) == 0);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    app_wait_for_input();
    assert(waits == 1 && sleeps == 0);
    SDL_Event event = {.type = SDL_USEREVENT};
    assert(SDL_PushEvent(&event) == 1);
    app_wait_for_input();
    assert(sleeps == 1 && waits == 1);
    assert(SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_USEREVENT, SDL_USEREVENT) == 1);
    SDL_Quit();
    puts("event_wait: timed wake and retained UI event ownership passed");
    return 0;
}
