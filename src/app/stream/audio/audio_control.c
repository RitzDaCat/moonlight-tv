#include "audio_control.h"

#include <math.h>
#include <string.h>

#define AUDIO_UNITY_GAIN 32768
#define AUDIO_MUTED_FLAG 65536
#define AUDIO_GAIN_RAMP_MS 5

static int command(int gain_db, bool muted, bool split) {
    if (gain_db < STREAM_AUDIO_MIN_DB) gain_db = STREAM_AUDIO_MIN_DB;
    if (gain_db > STREAM_AUDIO_MAX_DB) gain_db = STREAM_AUDIO_MAX_DB;
    /* Two potentially full-scale streams share output headroom. Keep this
     * stable while muting to avoid changing the other player's loudness. */
    float gain = powf(10.0f, (float) gain_db / 20.0f) * (split ? 0.5f : 1.0f);
    return (int) lroundf(gain * AUDIO_UNITY_GAIN) | (muted ? AUDIO_MUTED_FLAG : 0);
}

void stream_audio_control_set(stream_audio_control_t *control, int gain_db, bool muted, bool split) {
    SDL_AtomicSet(&control->command, command(gain_db, muted, split));
}

void stream_audio_control_init(stream_audio_control_t *control, int gain_db, bool muted, bool split) {
    int value = command(gain_db, muted, split);
    SDL_AtomicSet(&control->command, value);
    control->current_gain = control->ramp_target = value & AUDIO_MUTED_FLAG ? 0 : value;
    control->ramp_remaining = 0;
}

static int16_t scale(int16_t sample, int gain) {
    /* Both factors are bounded; the product fits signed32 bits. */
    return (int16_t) (((int32_t) sample * gain) / AUDIO_UNITY_GAIN);
}

void stream_audio_control_apply(stream_audio_control_t *control, int16_t *samples, size_t frames,
                                unsigned channels, unsigned sample_rate) {
    if (!samples || !channels || channels > 8 || !sample_rate || sample_rate > 48000 ||
        frames > SIZE_MAX / channels / sizeof(*samples)) return;
    int value = SDL_AtomicGet(&control->command);
    int target = value & AUDIO_MUTED_FLAG ? 0 : value;
    if (target != control->ramp_target) {
        control->ramp_target = target;
        control->ramp_remaining = (sample_rate * AUDIO_GAIN_RAMP_MS + 999) / 1000;
    }
    size_t frame = 0;
    while (frame < frames && control->ramp_remaining) {
        control->current_gain += (target - control->current_gain) / (int) control->ramp_remaining;
        --control->ramp_remaining;
        for (unsigned channel = 0; channel < channels; ++channel) {
            size_t index = frame * channels + channel;
            samples[index] = scale(samples[index], control->current_gain);
        }
        ++frame;
    }
    if (frame == frames) return;
    if (target == 0) {
        memset(samples + frame * channels, 0, (frames - frame) * channels * sizeof(*samples));
    } else if (target != AUDIO_UNITY_GAIN) {
        for (size_t index = frame * channels; index < frames * channels; ++index) {
            samples[index] = scale(samples[index], target);
        }
    }
}
