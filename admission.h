#ifndef ADMISSION_H
#define ADMISSION_H

#include "port.h"
#include <semaphore.h>
#include <stdatomic.h>

typedef struct {
    Port *ports;
    unsigned count, wait_seconds;
    int listener, wake_fd, status, started;
    atomic_int *stopping;
    sem_t free_ports;
    pthread_mutex_t table_lock;
    int revoked_token; /* The single acceptor reserved a slot the admin disabled. */
    pthread_rwlock_t forwarding;
    int forwarding_ready;
    pthread_t thread;
} Admission;

int admission_init(Admission *admission, Port *ports, unsigned count, int listener,
                   int wake_fd, unsigned wait_seconds, atomic_int *stopping);
int admission_start(Admission *admission);
int admission_reap(Admission *admission);
int admission_shutdown_port(Admission *admission, unsigned port);
int admission_up_port(Admission *admission, unsigned port);
void admission_stop(Admission *admission);
void admission_destroy(Admission *admission);

#endif
