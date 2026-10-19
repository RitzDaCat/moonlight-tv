#include "session_stats.h"

void session_stats_publish_video(session_stats_t *store, const VIDEO_STATS *video, const VIDEO_INFO *info) {
    SDL_AtomicLock(&store->lock);
    store->video = *video;
    store->info = *info;
    SDL_AtomicUnlock(&store->lock);
}

void session_stats_publish_audio(session_stats_t *store, const AUDIO_INFO *audio) {
    SDL_AtomicLock(&store->lock);
    store->audio = *audio;
    SDL_AtomicUnlock(&store->lock);
}

void session_stats_read(session_stats_t *store, VIDEO_STATS *video, VIDEO_INFO *info, AUDIO_INFO *audio) {
    SDL_AtomicLock(&store->lock);
    *video = store->video;
    *info = store->info;
    *audio = store->audio;
    SDL_AtomicUnlock(&store->lock);
}
