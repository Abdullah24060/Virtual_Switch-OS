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
    error = monotonic_cond_init(&port->not_empty);
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

static int enqueue_generation(Port *port, SharedFrame *frame, uint64_t generation)
{
    int added = -1;
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    if (port->state == PORT_ACTIVE && (generation == 0 || port->generation == generation)) {
        int empty = port->output.count == 0;
        added = output_enqueue(&port->output, frame, monotonic_ns());
        if (!added) port->dropped++;
        else if (empty) {
            check_thread(pthread_cond_signal(&port->not_empty), "signal sender");
        }
    }
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    return added;
}

int port_enqueue(Port *port, SharedFrame *frame)
{
    return enqueue_generation(port, frame, 0) == 1;
}

static int broadcast(const uint8_t mac[MAC_SIZE])
{
    for (unsigned i = 0; i < MAC_SIZE; i++) {
        if (mac[i] != 0xff) return 0;
    }
    return 1;
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

        unsigned index = (unsigned)(port - port->all_ports), old_port = 0;
        int learned = mac_learn(port->mac_table, frame.src, index, port->generation, &old_port);
        if (learned == MAC_ERROR) {
            perror("learn MAC");
            break;
        }
        if (learned == MAC_LIMIT) {
            check_thread(pthread_mutex_lock(&port->lock), "lock port");
            port->dropped++;
            check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
            if (printf("port security: port %u exceeded MAC limit\n", index + 1) < 0 ||
                fflush(stdout) == EOF) perror("print port security");
            break; /* Stop this port; the violating frame is never forwarded. */
        }
        if (learned == MAC_MOVED) {
            if (printf("MAC moved %02x:%02x:%02x:%02x:%02x:%02x port %u -> %u\n",
                       frame.src[0], frame.src[1], frame.src[2], frame.src[3],
                       frame.src[4], frame.src[5], old_port + 1, index + 1) < 0 ||
                fflush(stdout) == EOF) {
                perror("print MAC move");
                break;
            }
        }
        SharedFrame *shared = shared_frame_create(&frame);
        if (shared == NULL) {
            perror("allocate shared frame");
            break;
        }
        MacLocation destination;
        int flood = broadcast(frame.dst) || !mac_lookup(port->mac_table, frame.dst, &destination);
        if (!flood && destination.port != index) {
            /* A full known-destination queue drops, rather than flooding. */
            flood = enqueue_generation(&port->all_ports[destination.port], shared,
                                       destination.generation) == -1;
        }
        if (flood) {
            for (unsigned i = 0; i < port->num_ports; i++) {
                if (i != index) port_enqueue(&port->all_ports[i], shared);
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

/* Both waits release the port mutex. Only the timer grants a spent token. */
static void wait_to_send(Port *port)
{
    while (port->state == PORT_ACTIVE) {
        if (port->output.count == 0) {
            check_thread(pthread_cond_wait(&port->not_empty, &port->lock), "wait for frame");
        } else if (!port->send_token) {
            struct timespec deadline = monotonic_deadline(port->refill_at);
            int error = pthread_cond_timedwait(&port->not_empty, &port->lock, &deadline);
            if (error == ETIMEDOUT) {
                /* The timer can run just after this deadline. Wait for its
                   notification instead of retrying an expired timed wait. */
                while (!port->send_token && port->state == PORT_ACTIVE)
                    check_thread(pthread_cond_wait(&port->not_empty, &port->lock), "wait for refill");
            } else {
                check_thread(error, "wait for send token");
            }
        } else {
            break;
        }
    }
}

static void *send_frames(void *argument)
{
    Port *port = argument;
    for (;;) {
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        wait_to_send(port);
        if (port->state != PORT_ACTIVE) {
            check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
            break;
        }
        SharedFrame *shared = output_take(&port->output, monotonic_ns());
        port->send_token = 0;
        port->refill_at = 0; /* Arm the refill after the write completes. */
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");

        /* Never hold a port or frame lock during a socket write. */
        int result = frame_write(port->fd, &shared->frame);
        shared_frame_release(shared);
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        int refill = result == 0 && port->state == PORT_ACTIVE;
        if (result == 0) {
            port->frames_out++;
            if (refill) port->refill_at = monotonic_ns() + port->send_interval_ns;
        } else {
            port->dropped++;
        }
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
        if (refill) rate_timer_notify(port->rate_timer);
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
    port->send_token = 1; /* One frame can leave immediately; idle credits cannot build up. */
    port->refill_at = 0;
    port->frames_in = port->frames_out = port->dropped = 0;
    port->state = PORT_ACTIVE;
    port->generation++;
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

    /* Main is the only reaper. Join without holding this port's lock. */
    if (port->receiver_started) {
        check_thread(pthread_join(port->receiver, NULL), "join receiver");
        port->receiver_started = 0;
    }
    if (port->sender_started) {
        check_thread(pthread_join(port->sender, NULL), "join sender");
        port->sender_started = 0;
    }
    if (port->mac_table != NULL)
        mac_forget_port(port->mac_table, (unsigned)(port - port->all_ports), port->generation);
    int result = 1;
    if (close(port->fd) == -1) {
        perror("close port");
        result = -1;
    }
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    port->dropped += output_discard(&port->output);
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
