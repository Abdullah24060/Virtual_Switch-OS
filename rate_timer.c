#define _POSIX_C_SOURCE 200809L
#include "rate_timer.h"
#include "port.h"
#include "sync.h"

#include <errno.h>

uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("read monotonic clock");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

struct timespec monotonic_deadline(uint64_t ns)
{
    return (struct timespec){.tv_sec = (time_t)(ns / NS_PER_SECOND),
                             .tv_nsec = (long)(ns % NS_PER_SECOND)};
}

int monotonic_cond_init(pthread_cond_t *condition)
{
    pthread_condattr_t attr;
    int error = pthread_condattr_init(&attr);
    if (error != 0) return error;
    error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (error == 0) error = pthread_cond_init(condition, &attr);
    check_thread(pthread_condattr_destroy(&attr), "destroy condition attributes");
    return error;
}

/* Refill spent buckets and find the earliest deadline still outstanding. */
static uint64_t refill_ports(RateTimer *timer)
{
    uint64_t next = 0;
    for (unsigned i = 0; i < timer->count; i++) {
        Port *port = &timer->ports[i];
        check_thread(pthread_mutex_lock(&port->lock), "lock port for refill");
        if (port->state == PORT_ACTIVE && !port->send_token && port->refill_at != 0) {
            if (monotonic_ns() >= port->refill_at) {
                port->send_token = 1;
                check_thread(pthread_cond_signal(&port->not_empty), "wake rate-limited sender");
            } else if (next == 0 || port->refill_at < next) {
                next = port->refill_at;
            }
        }
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port after refill");
    }
    return next;
}

static void *run_timer(void *argument)
{
    RateTimer *timer = argument;
    check_thread(pthread_mutex_lock(&timer->lock), "lock rate timer");
    while (!timer->stopping) {
        timer->changed = 0;
        /* A sender can publish a new deadline while this scan is in progress. */
        check_thread(pthread_mutex_unlock(&timer->lock), "unlock rate timer");
        uint64_t next = refill_ports(timer);
        check_thread(pthread_mutex_lock(&timer->lock), "lock rate timer");
        while (!timer->stopping && !timer->changed) {
            if (next == 0) {
                check_thread(pthread_cond_wait(&timer->wake, &timer->lock), "wait for spent token");
            } else {
                struct timespec deadline = monotonic_deadline(next);
                int error = pthread_cond_timedwait(&timer->wake, &timer->lock, &deadline);
                if (error == ETIMEDOUT) break;
                check_thread(error, "wait for token refill");
            }
        }
    }
    check_thread(pthread_mutex_unlock(&timer->lock), "unlock rate timer");
    return NULL;
}

int rate_timer_start(RateTimer *timer, Port *ports, unsigned count)
{
    *timer = (RateTimer){.ports = ports, .count = count};
    int error = pthread_mutex_init(&timer->lock, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = monotonic_cond_init(&timer->wake);
    if (error == 0) {
        error = pthread_create(&timer->thread, NULL, run_timer, timer);
        if (error == 0) return 0;
        check_thread(pthread_cond_destroy(&timer->wake), "destroy rate condition");
    }
    check_thread(pthread_mutex_destroy(&timer->lock), "destroy rate timer lock");
    errno = error;
    return -1;
}

void rate_timer_notify(RateTimer *timer)
{
    check_thread(pthread_mutex_lock(&timer->lock), "lock rate timer");
    timer->changed = 1;
    check_thread(pthread_cond_signal(&timer->wake), "notify rate timer");
    check_thread(pthread_mutex_unlock(&timer->lock), "unlock rate timer");
}

void rate_timer_stop(RateTimer *timer)
{
    check_thread(pthread_mutex_lock(&timer->lock), "lock rate timer");
    timer->stopping = 1;
    check_thread(pthread_cond_signal(&timer->wake), "stop rate timer");
    check_thread(pthread_mutex_unlock(&timer->lock), "unlock rate timer");
    check_thread(pthread_join(timer->thread, NULL), "join rate timer");
    check_thread(pthread_cond_destroy(&timer->wake), "destroy rate condition");
    check_thread(pthread_mutex_destroy(&timer->lock), "destroy rate timer lock");
}
