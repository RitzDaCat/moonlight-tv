#pragma once
#include <SDL.h>

static inline void app_wait_for_input(void) {
    /* LVGL owns retained menu events; do not consume/requeue them or busy-spin
     * while its next timer is pending. An empty queue may wake on new input. */
    if (SDL_HasEvents(SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        SDL_Delay(1);
    } else {
        SDL_WaitEventTimeout(NULL, 1);
    }
}
