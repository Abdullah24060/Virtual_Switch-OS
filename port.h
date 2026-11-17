#ifndef PORT_H
#define PORT_H

#include "shared_frame.h"
#include "mac_table.h"
#include <stdint.h>

#define QUEUE_CAPACITY 64

typedef enum { PORT_FREE, PORT_ACTIVE, PORT_CLOSING } PortState;

typedef struct Port {
    /* The lock protects state, the queue and counters. */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    PortState state;
    SharedFrame *queue[QUEUE_CAPACITY];
    size_t head, count;
    uint64_t frames_in, frames_out, dropped;

    /* Main sets fd before starting workers, and closes it after joining them. */
    int fd;
    pthread_t receiver, sender;
    int receiver_started, sender_started; /* Used only by main. */

    /* Set once, before any workers start. */
    struct Port *all_ports;
    unsigned num_ports;
    int wake_fd;
    MacTable *mac_table;
    uint64_t generation; /* A new connection never inherits an old MAC mapping. */
} Port;

int port_init(Port *port, Port *all_ports, unsigned count, int wake_fd);
int port_start(Port *port, int fd);
void port_stop(Port *port);
int port_reap(Port *port, unsigned number); /* 1 = reaped, 0 = unchanged, -1 = error. */
void port_destroy(Port *port);
int port_is_free(Port *port);

/* Neither enqueue nor notification waits for buffer space. */
int port_enqueue(Port *port, SharedFrame *frame);
void notify_main(int fd, char event);

#endif
