#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include "frame.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define MAC_BUCKETS 32

typedef struct MacEntry {
    uint8_t mac[MAC_SIZE];
    uint16_t vlan;
    unsigned port;
    uint64_t generation, used;
    struct timespec seen;
    struct MacEntry *next;
} MacEntry;

typedef struct {
    pthread_mutex_t lock;
    MacEntry *head;
} MacBucket;

typedef struct {
    MacBucket buckets[MAC_BUCKETS];
    /* Only structural changes take this lock, not lookups or same-port refreshes. */
    pthread_mutex_t changes;
    size_t count, capacity;
    unsigned *per_port; /* Distinct MAC addresses, even across several VLANs on a trunk. */
    unsigned num_ports, per_port_limit, age_seconds;
    atomic_uint_fast64_t sequence;
    pthread_mutex_t aging_lock;
    pthread_cond_t aging_wake;
    pthread_t aging_thread;
    int aging_stop, aging_started;
} MacTable;

typedef struct {
    unsigned port;
    uint64_t generation;
} MacLocation;

enum { MAC_ERROR = -1, MAC_OK, MAC_MOVED, MAC_LIMIT };

int mac_table_init(MacTable *table, unsigned ports, size_t capacity,
                   unsigned limit, unsigned age_seconds);
int mac_start_aging(MacTable *table);
void mac_stop_aging(MacTable *table);
void mac_table_destroy(MacTable *table);
int mac_learn(MacTable *table, const uint8_t mac[MAC_SIZE], uint16_t vlan, unsigned port,
              uint64_t generation, unsigned *old_port);
int mac_lookup(MacTable *table, const uint8_t mac[MAC_SIZE], uint16_t vlan,
               MacLocation *location);
void mac_forget_port(MacTable *table, unsigned port, uint64_t generation);
void mac_age(MacTable *table);

#endif
