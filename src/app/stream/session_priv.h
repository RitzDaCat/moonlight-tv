#pragma once

#include "config.h"
#include <SDL_thread.h>
#include "ss4s.h"
#include "client.h"
#include "app_settings.h"
#include "stream/input/session_input.h"
#include "stream/session.h"
#include "embed_wrapper.h"
#include "mlc_slot.h"
#include "session_stats.h"
#include "stream/audio/audio_control.h"

typedef struct app_t app_t;

struct session_t {
    session_config_t config;

    app_t *app;

    STREAMING_STATE state;
    int display_width, display_height;

    stream_input_t input;
    /* SERVER_DATA and CONFIGURATION is cloned rather than referenced */
    SERVER_DATA *server;
    int app_id;
    char *app_name;
    bool interrupted;
    bool quitapp;
#if FEATURE_EMBEDDED_SHELL
    bool embed;
    embed_process_t *embed_process;
#endif
    SS4S_AudioCapabilities audio_cap;
    SS4S_VideoCapabilities video_cap;
    SDL_cond *cond;
    SDL_mutex *mutex;
    SDL_Thread *thread;
    SS4S_Player *player;
    session_stats_t stats;
    stream_audio_control_t audio_control;
    /* Stream library copy driving this session, index into mlc_slots */
    int slot;
    const mlc_api_t *mlc;
    /* Split screen: gamepads alternate between the sessions by app-wide id (even → slot 0, odd → slot 1),
     * each host numbering its own pads from 0 */
    bool split;
};

static inline bool session_owns_gamepad(const session_t *session, short gs_id) {
    return !session->split || (gs_id & 1) == session->slot;
}

/* App-wide gamepad id to the id this session's host sees */
static inline short session_host_gamepad_id(const session_t *session, short gs_id) {
    return session->split ? (short) (gs_id >> 1) : gs_id;
}

/* Host gamepad id back to the app-wide id, for rumble and LED requests */
static inline short session_app_gamepad_id(const session_t *session, short host_id) {
    return session->split ? (short) ((host_id << 1) | session->slot) : host_id;
}

/* App-wide gamepad mask to the mask this session's host sees */
static inline short session_host_gamepad_mask(const session_t *session, short mask) {
    if (!session->split) {
        return mask;
    }
    /* The protocol mask is16 bits. A connected pad15 sets the sign bit of
     * short; arithmetic right shifts must not keep this loop alive forever. */
    unsigned short active = (unsigned short) mask;
    short host_mask = 0;
    for (int host_id = 0; host_id < 8; host_id++) {
        if (active & (1U << ((host_id << 1) | session->slot))) {
            host_mask |= (short) (1 << host_id);
        }
    }
    return host_mask;
}

void session_set_state(session_t *session, STREAMING_STATE state);

#if FEATURE_EMBEDDED_SHELL
bool session_use_embedded(session_t *session);
#endif
