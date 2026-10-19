#include "unity.h"
#include <SDL.h>
#include "stream/session_stats.h"

static session_stats_t store;
static SDL_atomic_t done;

void setUp(void) {
    SDL_memset(&store, 0, sizeof(store));
    SDL_AtomicSet(&done, 0);
}
void tearDown(void) {}

static int publish_windows(void *unused) {
    (void) unused;
    for (uint32_t i = 1; i <= 10000; ++i) {
        VIDEO_STATS video = {.receivedFrames = i, .hostLatencyFrames = i,
                             .totalCaptureLatency = (uint64_t) i * 1000000};
        VIDEO_INFO info = {.width = (int) i, .height = -(int) i};
        session_stats_publish_video(&store, &video, &info);
        AUDIO_INFO audio = {.format = "Opus", .channels = "Stereo"};
        session_stats_publish_audio(&store, &audio);
    }
    SDL_AtomicSet(&done, 1);
    return 0;
}

static void test_concurrent_windows_remain_consistent(void) {
    SDL_Thread *writer = SDL_CreateThread(publish_windows, "stats-test", NULL);
    TEST_ASSERT_NOT_NULL(writer);
    bool consistent = true;
    VIDEO_STATS video;
    VIDEO_INFO info;
    AUDIO_INFO audio;
    do {
        session_stats_read(&store, &video, &info, &audio);
        consistent &= video.receivedFrames == video.hostLatencyFrames &&
                      video.totalCaptureLatency == (uint64_t) video.receivedFrames * 1000000 &&
                      info.width == (int) video.receivedFrames && info.height == -info.width;
    } while (!SDL_AtomicGet(&done));
    SDL_WaitThread(writer, NULL);
    TEST_ASSERT_TRUE(consistent);
    session_stats_read(&store, &video, &info, &audio);
    TEST_ASSERT_EQUAL_UINT32(10000, video.receivedFrames);
    TEST_ASSERT_EQUAL_STRING("Opus", audio.format);
    TEST_ASSERT_EQUAL_STRING("Stereo", audio.channels);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_concurrent_windows_remain_consistent);
    return UNITY_END();
}
