#ifndef RATE_TIMER_H
#define RATE_TIMER_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define NS_PER_SECOND UINT64_C(1000000000)
#define DEFAULT_SEND_RATE 1000
#define MAX_SEND_RATE 10000

struct Port;

typedef struct {
    struct Port *ports;
    unsigned count;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int changed, stopping;
} RateTimer;

uint64_t monotonic_ns(void);
struct timespec monotonic_deadline(uint64_t ns);
int monotonic_cond_init(pthread_cond_t *condition);

int rate_timer_start(RateTimer *timer, struct Port *ports, unsigned count);
void rate_timer_notify(RateTimer *timer);
void rate_timer_stop(RateTimer *timer);

#endif
