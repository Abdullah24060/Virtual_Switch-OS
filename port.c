#define _POSIX_C_SOURCE 200809L
#include "port.h"
#include "sync.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

void notify_main(int fd, char event)
{
    ssize_t n;
    do {
        n = write(fd, &event, 1);
    } while (n == -1 && errno == EINTR);
    /* A full pipe already wakes main; no event needs to wait for space. */
    if (n != 1 && !(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        perror("notify main");
        exit(EXIT_FAILURE);
    }
}

int port_init(Port *port, Port *all_ports, unsigned count, int wake_fd)
{
    int error = pthread_mutex_init(&port->lock, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_cond_init(&port->not_empty, NULL);
    if (error != 0) {
        check_thread(pthread_mutex_destroy(&port->lock), "destroy port lock");
        errno = error;
        return -1;
    }
    port->state = PORT_FREE;
    port->fd = -1;
    port->all_ports = all_ports;
    port->num_ports = count;
    port->wake_fd = wake_fd;
    return 0;
}

int port_is_free(Port *port)
{
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int available = port->state == PORT_FREE;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    return available;
}

void port_stop(Port *port)
{
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int changed = port->state == PORT_ACTIVE;
    if (changed) {
        port->state = PORT_CLOSING;
        check_thread(pthread_cond_broadcast(&port->not_empty), "wake sender");
        /* Wake both blocked read and write, but keep fd valid until both join. */
        if (shutdown(port->fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
            perror("shutdown port");
        }
    }
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    if (changed) {
        notify_main(port->wake_fd, 'P');
    }
}

int port_enqueue(Port *port, SharedFrame *frame)
{
    int added = 0;
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    if (port->state == PORT_ACTIVE) {
        if (port->count == QUEUE_CAPACITY) {
            port->dropped++;
        } else {
            /* Acquire ownership BEFORE the sender can see this pointer. */
            shared_frame_retain(frame);
            port->queue[(port->head + port->count) % QUEUE_CAPACITY] = frame;
            port->count++;
            added = 1;
            check_thread(pthread_cond_signal(&port->not_empty), "signal sender");
        }
    }
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    return added;
}

static void *receive_frames(void *argument)
{
    Port *port = argument;
    Frame frame;
    int result;

    while ((result = frame_read(port->fd, &frame)) == 1) {
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        int active = port->state == PORT_ACTIVE;
        if (active) port->frames_in++;
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
        if (!active) break;

        SharedFrame *shared = shared_frame_create(&frame);
        if (shared == NULL) {
            perror("allocate shared frame");
            break;
        }
        /* Until MAC learning is added, every destination is flooded. */
        for (unsigned i = 0; i < port->num_ports; i++) {
            if (&port->all_ports[i] != port) {
                port_enqueue(&port->all_ports[i], shared);
            }
        }
        shared_frame_release(shared); /* Release the receiver's ownership. */
    }
    if (result == -1) {
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        int active = port->state == PORT_ACTIVE;
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
        if (active) perror("host frame");
    }
    port_stop(port);
    return NULL;
}

static void *send_frames(void *argument)
{
    Port *port = argument;
    for (;;) {
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        while (port->count == 0 && port->state == PORT_ACTIVE) {
            check_thread(pthread_cond_wait(&port->not_empty, &port->lock), "wait for frame");
        }
        if (port->state != PORT_ACTIVE) {
            check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
            break;
        }
        SharedFrame *shared = port->queue[port->head];
        port->head = (port->head + 1) % QUEUE_CAPACITY;
        port->count--;
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");

        /* Never hold a port or frame lock during a socket write. */
        int result = frame_write(port->fd, &shared->frame);
        shared_frame_release(shared);
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        if (result == 0) port->frames_out++;
        else port->dropped++;
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
        if (result == -1) {
            port_stop(port);
            break;
        }
    }
    return NULL;
}

int port_start(Port *port, int fd)
{
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    port->fd = fd;
    port->head = port->count = 0;
    port->frames_in = port->frames_out = port->dropped = 0;
    port->state = PORT_ACTIVE;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");

    int error = pthread_create(&port->sender, NULL, send_frames, port);
    if (error == 0) {
        port->sender_started = 1;
        error = pthread_create(&port->receiver, NULL, receive_frames, port);
        if (error == 0) port->receiver_started = 1;
    }
    if (error != 0) {
        port_stop(port);
        errno = error;
        return -1;
    }
    return 0;
}

int port_reap(Port *port, unsigned number)
{
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int closing = port->state == PORT_CLOSING;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    if (!closing) return 0;

    /* Main is the only reaper; joins happen without holding any lock. */
    if (port->receiver_started) {
        check_thread(pthread_join(port->receiver, NULL), "join receiver");
        port->receiver_started = 0;
    }
    if (port->sender_started) {
        check_thread(pthread_join(port->sender, NULL), "join sender");
        port->sender_started = 0;
    }
    int result = 0;
    if (close(port->fd) == -1) {
        perror("close port");
        result = -1;
    }
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    while (port->count != 0) {
        shared_frame_release(port->queue[port->head]);
        port->head = (port->head + 1) % QUEUE_CAPACITY;
        port->count--;
        port->dropped++;
    }
    uint64_t in = port->frames_in, out = port->frames_out, dropped = port->dropped;
    port->fd = -1;
    port->state = PORT_FREE;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    if (printf("port %u disconnected in=%" PRIu64 " out=%" PRIu64 " dropped=%" PRIu64 "\n",
               number, in, out, dropped) < 0 || fflush(stdout) == EOF) {
        perror("print port counters");
        result = -1;
    }
    return result;
}

void port_destroy(Port *port)
{
    check_thread(pthread_cond_destroy(&port->not_empty), "destroy port condition");
    check_thread(pthread_mutex_destroy(&port->lock), "destroy port lock");
}
