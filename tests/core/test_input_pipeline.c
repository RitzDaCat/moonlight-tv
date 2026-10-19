/* Exercise the production sender with a recorded control transport. No game input. */
#define LC_DEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "Limelight-internal.h"

static unsigned sleep_budget_ms;
static void trackedSleepMs(int ms);
#define PltSleepMs trackedSleepMs
#include "../../core/moonlight-common-c/src/InputStream.c"
#undef PltSleepMs

int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
struct sockaddr_storage RemoteAddr;
SOCKADDR_LEN AddrLen;

static struct {
    uint32_t magic;
    uint8_t channel;
    int buttons;
    uint64_t time_us;
    bool more;
} sent[32];
static unsigned sent_count;

static void trackedSleepMs(int ms) {
    sleep_budget_ms += ms;
    PltSleepMs(ms);
}

static void quietLog(const char* format, ...) { (void)format; }

int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channel, uint32_t flags, bool more) {
    (void)length; (void)flags; (void)more;
    PNV_INPUT_HEADER header = (void*)data;
    assert(sent_count < 32);
    sent[sent_count].magic = LE32(header->magic);
    sent[sent_count].channel = channel;
    sent[sent_count].time_us = PltGetMicroseconds();
    sent[sent_count].more = more;
    if (header->magic == LE32(MULTI_CONTROLLER_MAGIC_GEN5)) {
        sent[sent_count].buttons = LE16(((PNV_MULTI_CONTROLLER_PACKET)data)->buttonFlags);
    }
    sent_count++;
    return 0;
}

void flushInputOnControlStream(void) {}
bool isControlDataInTransit(void) { return false; }

static void replay(bool sunshine, bool baseline) {
    memset(AppVersionQuad, 0, sizeof(AppVersionQuad));
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431;
    AppVersionQuad[3] = sunshine ? -1 : 0;
    memset(sent, 0, sizeof(sent));
    sent_count = sleep_budget_ms = 0;
    assert(initializeInputStream() == 0);
    initialized = true;
    uint64_t start = PltGetMicroseconds();
    assert(LiSendKeyboardEvent(0xA2, KEY_ACTION_UP, 0) == 0);
    assert(LiSendUtf8TextEvent("a\xc3\xa9", 3) == 0);
    assert(LiSendControllerEvent(A_FLAG, 0, 0, 0, 0, 0, 0) == 0);
    assert(LiSendControllerEvent(0, 0, 0, 0, 0, 0, 0) == 0);
    assert(PltCreateThread("InputReplay", inputSendThreadProc, NULL, &inputSendThread) == 0);
    LbqSignalQueueDrain(&packetQueue);
    PltJoinThread(&inputSendThread);
    initialized = false;
    assert(sent_count == 5);
    assert(sent[0].magic == KEY_UP_EVENT_MAGIC);
    assert(sent[1].magic == UTF8_TEXT_EVENT_MAGIC && sent[2].magic == UTF8_TEXT_EVENT_MAGIC);
    assert(sent[3].buttons == A_FLAG && sent[4].buttons == 0);
    assert(sent[2].more == baseline);
    if (sunshine && !baseline) {
        assert(sleep_budget_ms == 0);
        assert(sent[1].channel == CTRL_CHANNEL_KEYBOARD);
        assert(sent[2].channel == CTRL_CHANNEL_KEYBOARD);
    } else {
        assert(sleep_budget_ms == 50);
        assert(sent[1].channel == CTRL_CHANNEL_UTF8);
    }
    printf("%s controller_after_text_us=%llu scheduled_sleep_ms=%u\n",
           sunshine ? "Sunshine" : "GFE", (unsigned long long)(sent[3].time_us - start), sleep_budget_ms);
    destroyInputStream();
}

int main(int argc, char** argv) {
    bool baseline = argc > 1 && strcmp(argv[1], "--baseline") == 0;
    ListenerCallbacks.logMessage = quietLog;
    PltTicksInit();
    for (int i = 0; i < 5; ++i) replay(true, baseline);
    replay(false, baseline);
    return 0;
}
