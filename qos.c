#include "qos.h"

int output_enqueue(OutputQueues *output, SharedFrame *frame, uint64_t now)
{
    FrameQueue *queue = &output->classes[frame->frame.priority];
    if (queue->count == QUEUE_CAPACITY) return 0;
    size_t tail = (queue->head + queue->count) % QUEUE_CAPACITY;
    shared_frame_retain(frame);
    queue->items[tail] = (QueueItem){.frame = frame, .queued_at = now};
    queue->count++;
    output->count++;
    return 1;
}

static SharedFrame *take_head(OutputQueues *output, unsigned priority)
{
    FrameQueue *queue = &output->classes[priority];
    SharedFrame *frame = queue->items[queue->head].frame;
    queue->head = (queue->head + 1) % QUEUE_CAPACITY;
    queue->count--;
    output->count--;
    return frame;
}

SharedFrame *output_take(OutputQueues *output, uint64_t now)
{
    /* An overdue head takes precedence. Oldest first also protects high
       traffic when several queues stay above the aging threshold. */
    unsigned overdue = PRIORITY_COUNT;
    uint64_t longest_wait = 0;
    for (unsigned i = 0; i < PRIORITY_COUNT; i++) {
        FrameQueue *queue = &output->classes[i];
        if (queue->count == 0) continue;
        uint64_t waited = now - queue->items[queue->head].queued_at;
        if (waited >= QOS_AGING_NS && waited > longest_wait) {
            overdue = i;
            longest_wait = waited;
        }
    }
    if (overdue != PRIORITY_COUNT) return take_head(output, overdue);

    static const unsigned round[] = {0, 0, 0, 0, 0, 1, 1, 1, 2};
    const unsigned turns = sizeof round / sizeof round[0];
    for (unsigned i = 0; i < turns && output->count != 0; i++) {
        unsigned priority = round[output->turn];
        output->turn = (output->turn + 1) % turns;
        if (output->classes[priority].count != 0) return take_head(output, priority);
    }
    return NULL;
}

size_t output_discard(OutputQueues *output)
{
    size_t dropped = output->count;
    for (unsigned i = 0; i < PRIORITY_COUNT; i++) {
        while (output->classes[i].count != 0) shared_frame_release(take_head(output, i));
    }
    output->turn = 0;
    return dropped;
}
