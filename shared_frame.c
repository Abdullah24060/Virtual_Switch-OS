#include "shared_frame.h"
#include "sync.h"

#include <errno.h>
#include <stdlib.h>

SharedFrame *shared_frame_create(const Frame *frame)
{
    SharedFrame *shared = malloc(sizeof *shared);
    if (shared == NULL) {
        return NULL;
    }
    int error = pthread_mutex_init(&shared->lock, NULL);
    if (error != 0) {
        free(shared);
        errno = error;
        return NULL;
    }
    shared->frame = *frame;
    shared->references = 1;
    return shared;
}

void shared_frame_retain(SharedFrame *shared)
{
    check_thread(pthread_mutex_lock(&shared->lock), "lock frame");
    shared->references++;
    check_thread(pthread_mutex_unlock(&shared->lock), "unlock frame");
}

void shared_frame_release(SharedFrame *shared)
{
    check_thread(pthread_mutex_lock(&shared->lock), "lock frame");
    int last = --shared->references == 0;
    check_thread(pthread_mutex_unlock(&shared->lock), "unlock frame");
    if (last) {
        check_thread(pthread_mutex_destroy(&shared->lock), "destroy frame lock");
        free(shared);
    }
}
