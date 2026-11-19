#ifndef PORT_H
#define PORT_H

#include "qos.h"
#include "mac_table.h"
#include "rate_timer.h"
#include "stats.h"
#include "logger.h"
#include <stdint.h>

typedef enum { PORT_FREE, PORT_ACTIVE, PORT_CLOSING } PortState;

typedef struct Port {
    /* The lock protects state, forwarding settings, queues, token and counters. */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    PortState state;
    int enabled;
    OutputQueues output;
    int send_token;
    uint64_t refill_at;
    uint64_t frames_in, frames_out, dropped, mirrored;
    uint16_t vlan;
    int trunk, sniffer;
    struct Port *mirror_to;

    /* Main sets fd before starting workers, and closes it after joining them. */
    int fd;
    pthread_t receiver, sender;
    int receiver_started, sender_started; /* Used only by main. */

    /* Set once, before any workers start. */
    struct Port *all_ports;
    unsigned num_ports;
    int wake_fd;
    MacTable *mac_table;
    RateTimer *rate_timer;
    pthread_rwlock_t *forwarding;
    SharedStats *stats;
    Logger *logger;
    uint64_t send_interval_ns;
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
/* Caller holds this port's lock. */
void port_publish_stats(Port *port);
void port_discard_output(Port *port);

#endif
