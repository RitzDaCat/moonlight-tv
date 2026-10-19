#pragma once

#include <SDL_atomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STREAM_AUDIO_MIN_DB (-60)
#define STREAM_AUDIO_MAX_DB 0

/* UI owns command updates; one audio worker owns the remaining fields. */
typedef struct stream_audio_control_t {
    SDL_atomic_t command;
    int current_gain;
    int ramp_target;
    unsigned ramp_remaining;
} stream_audio_control_t;

void stream_audio_control_init(stream_audio_control_t *control, int gain_db, bool muted, bool split);
void stream_audio_control_set(stream_audio_control_t *control, int gain_db, bool muted, bool split);
void stream_audio_control_apply(stream_audio_control_t *control, int16_t *samples, size_t frames,
                                unsigned channels, unsigned sample_rate);
