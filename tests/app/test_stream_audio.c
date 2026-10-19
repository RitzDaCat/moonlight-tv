#include "unity.h"
#include "ss4s.h"

static size_t fed_bytes;
static int feeds;
static int nonzero_samples;
static SS4S_AudioOpenResult fake_open(SS4S_Player *player, const SS4S_AudioInfo *info) {
    (void) player;
    TEST_ASSERT_EQUAL_INT(240, info->samplesPerFrame);
    return SS4S_AUDIO_OPEN_OK;
}
static SS4S_AudioFeedResult fake_feed(SS4S_Player *player, const unsigned char *data, size_t size) {
    (void) player;
    feeds++;
    fed_bytes = size;
    nonzero_samples = 0;
    const int16_t *pcm = (const int16_t *) data;
    for (size_t i = 0; i < size / sizeof(*pcm); ++i) nonzero_samples += pcm[i] != 0;
    return SS4S_AUDIO_FEED_OK;
}
static bool fake_close(SS4S_Player *player) { (void) player; return true; }

#define SS4S_PlayerAudioOpen fake_open
#define SS4S_PlayerAudioFeed fake_feed
#define SS4S_PlayerAudioClose fake_close
#define ss4s_aud_callbacks tested_aud_callbacks
#define audio_stream_info tested_audio_info
#include "../../src/app/stream/audio/session_audio.c"

static session_t test_session;
static OPUS_MULTISTREAM_CONFIGURATION config;
void setUp(void) {
    SDL_memset(&test_session, 0, sizeof(test_session));
    SDL_memset(&config, 0, sizeof(config));
    config.sampleRate = 48000;
    config.channelCount = 2;
    config.streams = 1;
    config.coupledStreams = 1;
    config.mapping[0] = 0;
    config.mapping[1] = 1;
    config.samplesPerFrame = 240;
    feeds = 0;
    fed_bytes = 0;
    nonzero_samples = 0;
}
void tearDown(void) { aud_cleanup(); }

static void test_invalid_opus_never_reaches_backend(void) {
    TEST_ASSERT_EQUAL_INT(0, aud_init(AUDIO_CONFIGURATION_STEREO, &config, &test_session, 0));
    char packet[] = {(char) 0xff};
    aud_feed(packet, sizeof(packet));
    TEST_ASSERT_EQUAL_INT(0, feeds);
}
static void test_valid_opus_uses_decoded_sample_count(void) {
    TEST_ASSERT_EQUAL_INT(0, aud_init(AUDIO_CONFIGURATION_STEREO, &config, &test_session, 0));
    int error;
    OpusEncoder *encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    TEST_ASSERT_EQUAL_INT(OPUS_OK, error);
    opus_int16 silence[480] = {0};
    unsigned char packet[1024];
    int length = opus_encode(encoder, silence, 240, packet, sizeof(packet));
    opus_encoder_destroy(encoder);
    TEST_ASSERT_TRUE(length > 0);
    aud_feed((char *) packet, length);
    TEST_ASSERT_EQUAL_INT(1, feeds);
    TEST_ASSERT_EQUAL_UINT(960, fed_bytes);
}
static void test_invalid_frame_size_rejected(void) {
    config.samplesPerFrame = 0;
    TEST_ASSERT_TRUE(aud_init(AUDIO_CONFIGURATION_STEREO, &config, &test_session, 0) != 0);
    config.samplesPerFrame = 5761;
    TEST_ASSERT_TRUE(aud_init(AUDIO_CONFIGURATION_STEREO, &config, &test_session, 0) != 0);
}
static void test_muted_split_stream_keeps_feeding_and_unmutes_without_reinit(void) {
    test_session.split = true;
    test_session.audio_cap.codecs = SS4S_AUDIO_OPUS;
    stream_audio_control_init(&test_session.audio_control, 0, true, true);
    TEST_ASSERT_EQUAL_INT(0, aud_init(AUDIO_CONFIGURATION_STEREO, &config, &test_session, 0));
    int error;
    OpusEncoder *encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    TEST_ASSERT_EQUAL_INT(OPUS_OK, error);
    opus_int16 tone[480];
    for (unsigned i = 0; i < 480; ++i) tone[i] = i & 16 ? 10000 : -10000;
    unsigned char packet[1024];
    int length = opus_encode(encoder, tone, 240, packet, sizeof(packet));
    TEST_ASSERT_TRUE(length > 0);
    aud_feed((char *) packet, length);
    TEST_ASSERT_EQUAL_INT(1, feeds);
    TEST_ASSERT_EQUAL_UINT(960, fed_bytes);
    TEST_ASSERT_EQUAL_INT(0, nonzero_samples);
    stream_audio_control_set(&test_session.audio_control, -6, false, true);
    length = opus_encode(encoder, tone, 240, packet, sizeof(packet));
    TEST_ASSERT_TRUE(length > 0);
    aud_feed((char *) packet, length);
    TEST_ASSERT_EQUAL_INT(2, feeds);
    TEST_ASSERT_EQUAL_UINT(960, fed_bytes);
    TEST_ASSERT_TRUE(nonzero_samples > 0);
    opus_encoder_destroy(encoder);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_opus_never_reaches_backend);
    RUN_TEST(test_valid_opus_uses_decoded_sample_count);
    RUN_TEST(test_invalid_frame_size_rejected);
    RUN_TEST(test_muted_split_stream_keeps_feeding_and_unmutes_without_reinit);
    return UNITY_END();
}
