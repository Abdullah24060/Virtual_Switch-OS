#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include "frame.h"
#include <pthread.h>

typedef struct {
    Frame frame;                 /* Immutable after creation. */
    pthread_mutex_t lock;
    unsigned references;
} SharedFrame;

/* The creator owns one reference until it finishes enqueueing the frame. */
SharedFrame *shared_frame_create(const Frame *frame);
void shared_frame_retain(SharedFrame *shared);
void shared_frame_release(SharedFrame *shared);

#endif
