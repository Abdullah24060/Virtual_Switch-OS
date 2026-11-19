#ifndef STATS_H
#define STATS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t in, out, dropped, mirrored;
} PortCounters;

typedef struct {
    pthread_mutex_t lock;
    unsigned count;
    int running;
    PortCounters ports[];
} SharedStats;

int stats_create(const char *name, unsigned count, SharedStats **result);
int stats_open(const char *name, SharedStats **result);
int stats_close(SharedStats *stats);
int stats_stop(SharedStats *stats);
void stats_update(SharedStats *stats, unsigned port, PortCounters counters);
int stats_snapshot(SharedStats *stats, PortCounters *counters);

#endif
