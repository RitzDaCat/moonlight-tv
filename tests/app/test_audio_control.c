#include "stream/audio/audio_control.h"
#include <stdio.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
    stream_audio_control_t first, second;
    int16_t samples[480];
    stream_audio_control_init(&first, 0, false, false);
    for (unsigned i=0; i<480; ++i) samples[i] = i&1 ? 32767 : -32768;
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    assert(samples[0] == -32768 && samples[1] == 32767);

    stream_audio_control_init(&first, 0, false, true);
    stream_audio_control_init(&second, 0, false, true);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    assert(samples[0] == 10000 && samples[479] == 10000);
    stream_audio_control_set(&first, 0, true, true);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&first, samples, 120, 2, 48000);
    assert(samples[0] < 10000 && samples[238] > 0 && samples[238] == samples[239]);
    stream_audio_control_apply(&first, samples, 120, 2, 48000);
    assert(samples[238] == 0 && samples[239] == 0);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    assert(samples[0] == 0 && samples[479] == 0);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&second, samples, 240, 2, 48000);
    assert(samples[0] == 10000 && samples[479] == 10000);

    stream_audio_control_set(&first, 0, false, true);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    assert(samples[0] > 0 && samples[0] < 10000 && samples[479] == 10000);
    stream_audio_control_set(&first, -6, false, true);
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    for (unsigned i=0; i<480; ++i) samples[i] = 20000;
    stream_audio_control_apply(&first, samples, 240, 2, 48000);
    assert(samples[0] >= 5010 && samples[0] <= 5020);
    stream_audio_control_init(&first, 90, false, false);
    samples[0] = -32768;
    stream_audio_control_apply(&first, samples, 1, 1, 48000);
    assert(samples[0] == -32768);
    stream_audio_control_init(&first, -90, false, false);
    samples[0] = 32767;
    stream_audio_control_apply(&first, samples, 1, 1, 48000);
    assert(samples[0] >= 31 && samples[0] <= 33);
    puts("audio controls: isolation, mute/unmute ramps, gain bounds, headroom and PCM preservation passed");
    return 0;
}
