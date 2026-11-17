#ifndef SYNC_H
#define SYNC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pthread functions return an error number instead of setting errno. */
static inline void check_thread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

#endif
