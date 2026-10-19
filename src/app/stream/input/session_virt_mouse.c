#include "session_virt_mouse.h"
#include "session_input.h"
#include "stream/session_priv.h"

#include <SDL_stdinc.h>

static short calc_mouse_movement(short axis) {
    int magnitude = axis < 0 ? -(int) axis : axis;
    if (magnitude <= 4096) {
        return 0;
    }
    return (short) (SDL_sqrt(magnitude - 4096) * (axis > 0 ? 1 : -1));
}

static short movement_delta(double value) {
    if (value == 0.0) {
        return 0;
    }
    return (short) (SDL_fabs(value) > 1.0 ? value : (value > 0 ? 1 : -1));
}

void vmouse_set_running(session_input_vmouse_t *vmouse, bool running) {
    vmouse->running = running;
    if (!running) {
        vmouse_set_vector(vmouse, 0, 0);
        vmouse_set_trigger(vmouse, 0, 0);
        vmouse->state.modifier = false;
    }
}

void session_input_set_vmouse_active(session_input_vmouse_t *vmouse, bool active) {
    vmouse->state.active = active;
    if (!active) {
        vmouse_set_vector(vmouse, 0, 0);
        vmouse_set_trigger(vmouse, 0, 0);
    }
}

bool session_input_is_vmouse_active(session_input_vmouse_t *vmouse) {
    return vmouse->state.active;
}

void vmouse_set_vector(session_input_vmouse_t *vmouse, short x, short y) {
    vmouse->state.x = calc_mouse_movement(x);
    vmouse->state.y = calc_mouse_movement((short) -SDL_max(y, -32767));
}

void vmouse_set_trigger(session_input_vmouse_t *vmouse, unsigned char l, unsigned char r) {
    bool ldown = vmouse->running && vmouse->state.active && l > 64;
    bool rdown = vmouse->running && vmouse->state.active && r > 64;
    if (vmouse->state.l != ldown) {
        vmouse->session->mlc->LiSendMouseButtonEvent(
                ldown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    if (vmouse->state.r != rdown) {
        vmouse->session->mlc->LiSendMouseButtonEvent(
                rdown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    }
    vmouse->state.l = ldown;
    vmouse->state.r = rdown;
}

void vmouse_set_modifier(session_input_vmouse_t *vmouse, bool value) {
    vmouse->state.modifier = value;
}

void vmouse_update(session_input_vmouse_t *vmouse) {
    if (!vmouse->running || !vmouse->state.active ||
        (!vmouse->state.x && !vmouse->state.y)) {
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (!SDL_TICKS_PASSED(now, vmouse->next_tick)) {
        return;
    }
    double x = vmouse->state.x / 28.0;
    double y = vmouse->state.y / 28.0;
    if (vmouse->state.modifier) {
        short scroll = movement_delta(-y / 20.0);
        if (scroll != 0) {
            vmouse->session->mlc->LiSendHighResScrollEvent(scroll);
        }
    } else {
        vmouse->session->mlc->LiSendMouseMoveEvent(movement_delta(x), movement_delta(y));
    }
    double magnitude = SDL_max(SDL_fabs(x), SDL_fabs(y));
    uint32_t interval = (uint32_t) SDL_max(5.0, SDL_min(5.0 / magnitude, 20.0));
    vmouse->next_tick = now + interval;
}
