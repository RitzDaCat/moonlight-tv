#define LC_DEBUG
#include "Limelight-internal.h"
#include <assert.h>
#include <stdio.h>

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static LINKED_BLOCKING_QUEUE queue;
static LINKED_BLOCKING_QUEUE_ENTRY entry;
static int value = 42;

static void produce(void* ignored) {
    (void)ignored;
    PltSleepMs(2);
    assert(LbqOfferQueueItem(&queue, &value, &entry) == LBQ_SUCCESS);
}

static void drain(void* ignored) {
    (void)ignored;
    PltSleepMs(2);
    LbqSignalQueueDrain(&queue);
}

int main(void) {
    PltTicksInit();
    assert(LbqInitializeLinkedBlockingQueue(&queue, 8) == 0);
    assert(queue.timedWaitSupported);
    assert(LbqOfferQueueItem(&queue, &value, &entry) == LBQ_SUCCESS);
    uint64_t start = PltGetMicroseconds();
    LbqWaitForQueueData(&queue, 100);
    printf("already_queued_wait_us=%llu\n", (unsigned long long)(PltGetMicroseconds()-start));
    void* item;
    assert(LbqPollQueueElement(&queue, &item) == LBQ_SUCCESS && item == &value);
    PLT_THREAD worker;
    assert(PltCreateThread("WaitProducer", produce, NULL, &worker) == 0);
    start = PltGetMicroseconds();
    LbqWaitForQueueData(&queue, 100);
    printf("producer_wake_us=%llu\n", (unsigned long long)(PltGetMicroseconds()-start));
    PltJoinThread(&worker);
    assert(LbqPollQueueElement(&queue, &item) == LBQ_SUCCESS && item == &value);
    start = PltGetMicroseconds();
    LbqWaitForQueueData(&queue, 2);
    assert(LbqPollQueueElement(&queue, &item) == LBQ_NO_ELEMENT);
    printf("empty_timeout_us=%llu\n", (unsigned long long)(PltGetMicroseconds()-start));
    assert(PltCreateThread("WaitDrain", drain, NULL, &worker) == 0);
    LbqWaitForQueueData(&queue, 100);
    PltJoinThread(&worker);
    assert(queue.draining);
    assert(LbqWaitForQueueElement(&queue, &item) == LBQ_INTERRUPTED);
    assert(LbqDestroyLinkedBlockingQueue(&queue) == NULL);
    return 0;
}
