#define _GNU_SOURCE
#include "admission.h"
#include "sync.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void close_client(int client)
{
    if (close(client) == -1) {
        perror("close waiting host");
        exit(EXIT_FAILURE);
    }
}

static int admit_host(Admission *admission, int client)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) {
        int error = errno;
        close_client(client);
        errno = error;
        return -1;
    }
    deadline.tv_sec += admission->wait_seconds;
    int result;
    for (;;) {
        do {
            result = sem_timedwait(&admission->free_ports, &deadline);
        } while (result == -1 && errno == EINTR && !atomic_load(admission->stopping));
        if (result == -1) {
            if (errno != ETIMEDOUT && !atomic_load(admission->stopping)) {
                int error = errno;
                close_client(client);
                errno = error;
                return -1;
            }
            if (!atomic_load(admission->stopping)) {
                const char reply[] = "switch full\n";
                if (write_full(client, reply, sizeof reply - 1) == -1 &&
                    errno != EPIPE && errno != ECONNRESET) perror("send switch full");
            }
            close_client(client);
            return 0;
        }
        if (atomic_load(admission->stopping)) {
            close_client(client);
            return 0; /* Shutdown's extra semaphore post only wakes the waiter. */
        }
        check_thread(pthread_mutex_lock(&admission->table_lock), "lock port table");
        if (!admission->revoked_token) break;
        admission->revoked_token = 0;
        check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
    }
    unsigned i;
    for (i = 0; i < admission->count; i++) {
        if (port_is_free(&admission->ports[i])) break;
    }
    if (i == admission->count) {
        /* Only shutdown can add a token without freeing a real slot. */
        check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
        close_client(client);
        errno = EINVAL;
        return -1;
    }
    result = port_start(&admission->ports[i], client); /* Port now owns client. */
    if (result == 0 && (printf("port %u connected\n", i + 1) < 0 || fflush(stdout) == EOF))
        result = -1;
    check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
    return result;
}

static void *accept_hosts(void *argument)
{
    Admission *admission = argument;
    while (!atomic_load(admission->stopping)) {
        int client = accept(admission->listener, NULL, NULL);
        if (client == -1) {
            if (atomic_load(admission->stopping)) break;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            admission->status = 1;
            break;
        }
        if (admit_host(admission, client) == -1) {
            perror("admit host");
            admission->status = 1;
            break;
        }
    }
    atomic_store(admission->stopping, 1);
    notify_main(admission->wake_fd, 'S');
    return NULL;
}

int admission_init(Admission *admission, Port *ports, unsigned count, int listener,
                   int wake_fd, unsigned wait_seconds, atomic_int *stopping)
{
    memset(admission, 0, sizeof *admission);
    admission->ports = ports;
    admission->count = count;
    admission->listener = listener;
    admission->wake_fd = wake_fd;
    admission->wait_seconds = wait_seconds;
    admission->stopping = stopping;
    if (sem_init(&admission->free_ports, 0, count) == -1) return -1;
    int error = pthread_mutex_init(&admission->table_lock, NULL);
    if (error != 0) {
        if (sem_destroy(&admission->free_ports) == -1) perror("destroy port semaphore");
        errno = error;
        return -1;
    }
    pthread_rwlockattr_t attr;
    error = pthread_rwlockattr_init(&attr);
    if (error == 0) {
        /* A waiting admin writer should get a turn even under steady traffic. */
        error = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        if (error == 0) error = pthread_rwlock_init(&admission->forwarding, &attr);
        check_thread(pthread_rwlockattr_destroy(&attr), "destroy forwarding attributes");
    }
    if (error != 0) {
        admission_destroy(admission);
        errno = error;
        return -1;
    }
    admission->forwarding_ready = 1;
    for (unsigned i = 0; i < count; i++) ports[i].forwarding = &admission->forwarding;
    return 0;
}

int admission_start(Admission *admission)
{
    int error = pthread_create(&admission->thread, NULL, accept_hosts, admission);
    if (error != 0) {
        errno = error;
        return -1;
    }
    admission->started = 1;
    return 0;
}

int admission_reap(Admission *admission)
{
    int result = 0;
    check_thread(pthread_mutex_lock(&admission->table_lock), "lock port table");
    for (unsigned i = 0; i < admission->count; i++) {
        int reaped = port_reap(&admission->ports[i], i + 1);
        if (reaped == -1) result = -1;
        if (reaped == 1 && port_is_free(&admission->ports[i]) &&
            sem_post(&admission->free_ports) == -1) {
            perror("release port token");
            result = -1;
        }
    }
    check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
    return result;
}

int admission_shutdown_port(Admission *admission, unsigned index)
{
    Port *port = &admission->ports[index];
    check_thread(pthread_mutex_lock(&admission->table_lock), "lock port table");
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int was_enabled = port->enabled;
    int was_free = port->state == PORT_FREE;
    port->enabled = 0;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    int result = 0;
    if (was_enabled && was_free) {
        do {
            result = sem_trywait(&admission->free_ports);
        } while (result == -1 && errno == EINTR);
        if (result == -1 && errno == EAGAIN) {
            admission->revoked_token = 1;
            result = 0;
        }
    } else if (was_enabled) {
        port_stop(port);
        if (port_reap(port, index + 1) == -1) result = -1;
    }
    check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
    return result;
}

int admission_up_port(Admission *admission, unsigned index)
{
    Port *port = &admission->ports[index];
    check_thread(pthread_mutex_lock(&admission->table_lock), "lock port table");
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int result = 0;
    if (!port->enabled) {
        result = sem_post(&admission->free_ports);
        if (result == 0) port->enabled = 1;
    }
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    check_thread(pthread_mutex_unlock(&admission->table_lock), "unlock port table");
    return result;
}

void admission_stop(Admission *admission)
{
    if (!admission->started) return;
    atomic_store(admission->stopping, 1);
    /* Interrupt accept() and sem_timedwait() without waiting for the timeout. */
    if (shutdown(admission->listener, SHUT_RDWR) == -1 && errno != ENOTCONN)
        perror("shutdown listener");
    if (sem_post(&admission->free_ports) == -1) {
        perror("wake admission waiter");
        exit(EXIT_FAILURE);
    }
    check_thread(pthread_join(admission->thread, NULL), "join admission thread");
    admission->started = 0;
}

void admission_destroy(Admission *admission)
{
    if (admission->forwarding_ready)
        check_thread(pthread_rwlock_destroy(&admission->forwarding), "destroy forwarding lock");
    check_thread(pthread_mutex_destroy(&admission->table_lock), "destroy port table lock");
    if (sem_destroy(&admission->free_ports) == -1) {
        perror("destroy port semaphore");
        exit(EXIT_FAILURE);
    }
}
