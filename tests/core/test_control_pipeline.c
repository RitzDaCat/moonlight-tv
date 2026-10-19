#define LC_DEBUG
#include "Limelight-internal.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
static bool progress;
static int recordedService(ENetHost*, ENetEvent*, enet_uint32);
#define enet_host_service recordedService
#include "../../core/moonlight-common-c/src/ControlStream.c"
#undef enet_host_service

static int recordedService(ENetHost* host, ENetEvent* event, enet_uint32 timeout) {
    (void)event; (void)timeout;
    host->serviceTime = enet_time_get();
    if (progress) {
        while (!enet_list_empty(&peer->outgoingSendReliableCommands)) {
            ENetOutgoingCommand* cmd = enet_list_remove(enet_list_begin(&peer->outgoingSendReliableCommands));
            enet_list_insert(enet_list_end(&peer->sentReliableCommands), cmd);
            host->totalSentPackets++;
        }
    }
    return 0;
}

static void completeSend(void* ignored) {
    (void)ignored;
    usleep(250);
    PltLockMutex(&enetMutex);
    progress = true;
    recordedService(client, NULL, 0);
    PltBroadcastConditionVariable(&enetSendProgress);
    PltUnlockMutex(&enetMutex);
}

int main(void) {
    PltTicksInit();
    AppVersionQuad[0] = 5; AppVersionQuad[3] = -1;
    assert(enet_initialize() == 0);
    assert(initializeControlStream() == 0);
    assert(enetSendProgressTimed);
    client = enet_host_create(AF_INET, NULL, 1, CTRL_CHANNEL_COUNT, 0, 0);
    assert(client != NULL);
    ENetAddress address;
    memset(&address, 0, sizeof(address));
    assert(enet_address_set_host(&address, "127.0.0.1") == 0);
    assert(enet_address_set_port(&address, 9) == 0);
    peer = enet_host_connect(client, &address, CTRL_CHANNEL_COUNT, 0);
    assert(peer != NULL);
    peer->state = ENET_PEER_STATE_CONNECTED;
    peer->nextTimeout = enet_time_get() + 50;
    PLT_THREAD worker;
    assert(PltCreateThread("SendProgress", completeSend, NULL, &worker) == 0);
    uint64_t start = PltGetMicroseconds();
    assert(sendMessageEnet(0x206, 1, "x", CTRL_CHANNEL_KEYBOARD, ENET_PACKET_FLAG_RELIABLE, false));
    printf("controlled_send_wakeup_us=%llu\n", (unsigned long long)(PltGetMicroseconds()-start));
    PltJoinThread(&worker);
    assert(!enet_list_empty(&peer->sentReliableCommands));
    progress = false;
    start = PltGetMicroseconds();
    assert(sendMessageEnet(0x206, 1, "y", CTRL_CHANNEL_KEYBOARD, ENET_PACKET_FLAG_RELIABLE, false));
    printf("controlled_backpressure_timeout_us=%llu\n", (unsigned long long)(PltGetMicroseconds()-start));
    assert(!enet_list_empty(&peer->outgoingSendReliableCommands));
    enet_host_destroy(client);
    client = NULL; peer = NULL;
    stopping = true;
    destroyControlStream();
    enet_deinitialize();
    return 0;
}
