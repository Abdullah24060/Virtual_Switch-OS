#ifndef QOS_H
#define QOS_H

#include "shared_frame.h"

#define PRIORITY_COUNT 3
#define QUEUE_CAPACITY 64
#define QOS_AGING_NS UINT64_C(200000000)

typedef struct {
    SharedFrame *frame;
    uint64_t queued_at;
} QueueItem;

typedef struct {
    QueueItem items[QUEUE_CAPACITY];
    size_t head, count;
} FrameQueue;

typedef struct {
    FrameQueue classes[PRIORITY_COUNT];
    size_t count;
    unsigned turn;
} OutputQueues;

/* Call under the port mutex. Enqueue retains a reference; take transfers it. */
int output_enqueue(OutputQueues *output, SharedFrame *frame, uint64_t now);
SharedFrame *output_take(OutputQueues *output, uint64_t now);
size_t output_discard(OutputQueues *output);

#endif
