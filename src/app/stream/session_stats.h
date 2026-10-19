#pragma once

#include <SDL_atomic.h>
#include "stream/session.h"

/* Workers publish complete windows; the UI copies them under this short lock. */
typedef struct session_stats_t {
    SDL_SpinLock lock;
    VIDEO_STATS video;
    VIDEO_INFO info;
    AUDIO_INFO audio;
} session_stats_t;

void session_stats_publish_video(session_stats_t *store, const VIDEO_STATS *video, const VIDEO_INFO *info);
void session_stats_publish_audio(session_stats_t *store, const AUDIO_INFO *audio);
void session_stats_read(session_stats_t *store, VIDEO_STATS *video, VIDEO_INFO *info, AUDIO_INFO *audio);
