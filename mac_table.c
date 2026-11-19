#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"
#include "sync.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned bucket_for(const uint8_t mac[MAC_SIZE])
{
    /* Keep the same MAC's VLAN entries together for the per-port MAC limit. */
    unsigned hash = 2166136261u;
    for (unsigned i = 0; i < MAC_SIZE; i++) hash = (hash ^ mac[i]) * 16777619u;
    return hash % MAC_BUCKETS;
}

static struct timespec now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) == -1) {
        perror("read monotonic clock");
        exit(EXIT_FAILURE);
    }
    return time;
}

static int expired(MacTable *table, MacEntry *entry, struct timespec time)
{
    time_t seconds = time.tv_sec - entry->seen.tv_sec;
    return seconds > table->age_seconds ||
           (seconds == table->age_seconds && time.tv_nsec >= entry->seen.tv_nsec);
}

static MacEntry *find(MacBucket *bucket, const uint8_t mac[MAC_SIZE], uint16_t vlan)
{
    for (MacEntry *entry = bucket->head; entry != NULL; entry = entry->next) {
        if (entry->vlan == vlan && memcmp(entry->mac, mac, MAC_SIZE) == 0) return entry;
    }
    return NULL;
}

static int has_address(const MacBucket *bucket, const uint8_t mac[MAC_SIZE],
                       unsigned port, const MacEntry *skip)
{
    for (const MacEntry *entry = bucket->head; entry != NULL; entry = entry->next) {
        if (entry != skip && entry->port == port && memcmp(entry->mac, mac, MAC_SIZE) == 0)
            return 1;
    }
    return 0;
}

static void touch(MacTable *table, MacEntry *entry, struct timespec time)
{
    entry->seen = time;
    entry->used = atomic_fetch_add(&table->sequence, 1) + 1;
}

/* Caller holds changes and the entry's bucket lock. */
static void remove_entry(MacTable *table, MacBucket *bucket, MacEntry **link)
{
    MacEntry *entry = *link;
    *link = entry->next;
    if (!has_address(bucket, entry->mac, entry->port, NULL)) table->per_port[entry->port]--;
    table->count--;
    free(entry);
}

static void age_locked(MacTable *table, struct timespec time)
{
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];
        check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
        MacEntry **link = &bucket->head;
        while (*link != NULL) {
            if (expired(table, *link, time)) remove_entry(table, bucket, link);
            else link = &(*link)->next;
        }
        check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    }
}

void mac_age(MacTable *table)
{
    check_thread(pthread_mutex_lock(&table->changes), "lock MAC changes");
    age_locked(table, now());
    check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
}

int mac_lookup(MacTable *table, const uint8_t mac[MAC_SIZE], uint16_t vlan,
               MacLocation *location)
{
    MacBucket *bucket = &table->buckets[bucket_for(mac)];
    int found = 0;
    check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
    MacEntry *entry = find(bucket, mac, vlan);
    if (entry != NULL && !expired(table, entry, now())) {
        location->port = entry->port;
        location->generation = entry->generation;
        entry->used = atomic_fetch_add(&table->sequence, 1) + 1;
        found = 1;
    }
    check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    return found;
}

/* Exact LRU needs a stable snapshot. Only eviction locks all buckets. */
static void evict_oldest(MacTable *table)
{
    for (unsigned i = 0; i < MAC_BUCKETS; i++)
        check_thread(pthread_mutex_lock(&table->buckets[i].lock), "lock eviction bucket");
    MacEntry **oldest = NULL;
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        for (MacEntry **link = &table->buckets[i].head; *link != NULL; link = &(*link)->next) {
            if (oldest == NULL || (*link)->used < (*oldest)->used) oldest = link;
        }
    }
    if (oldest != NULL)
        remove_entry(table, &table->buckets[bucket_for((*oldest)->mac)], oldest);
    for (unsigned i = MAC_BUCKETS; i > 0; i--)
        check_thread(pthread_mutex_unlock(&table->buckets[i - 1].lock), "unlock eviction bucket");
}

int mac_learn(MacTable *table, const uint8_t mac[MAC_SIZE], uint16_t vlan, unsigned port,
              uint64_t generation, unsigned *old_port)
{
    MacBucket *bucket = &table->buckets[bucket_for(mac)];
    check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
    MacEntry *entry = find(bucket, mac, vlan);
    if (entry != NULL && entry->port == port && entry->generation == generation) {
        touch(table, entry, now());
        check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
        return MAC_OK;
    }
    check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");

    check_thread(pthread_mutex_lock(&table->changes), "lock MAC changes");
    if (table->count == table->capacity || table->per_port[port] >= table->per_port_limit)
        age_locked(table, now());
    check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
    entry = find(bucket, mac, vlan); /* Recheck after acquiring structural ownership. */
    int result = MAC_OK;
    if (entry != NULL && entry->port == port && entry->generation == generation) {
        touch(table, entry, now());
    } else if (table->per_port[port] >= table->per_port_limit &&
               !has_address(bucket, mac, port, NULL)) {
        result = MAC_LIMIT;
    } else if (entry != NULL) {
        *old_port = entry->port;
        if (!has_address(bucket, mac, entry->port, entry)) table->per_port[entry->port]--;
        if (!has_address(bucket, mac, port, NULL)) table->per_port[port]++;
        entry->port = port;
        entry->generation = generation;
        touch(table, entry, now());
        result = MAC_MOVED;
    } else {
        entry = malloc(sizeof *entry);
        if (entry == NULL) {
            result = MAC_ERROR;
        } else {
            /* Drop the one bucket before taking all eviction locks in order. */
            check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
            if (table->count == table->capacity) evict_oldest(table);
            check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
            memcpy(entry->mac, mac, MAC_SIZE);
            entry->vlan = vlan;
            entry->port = port;
            entry->generation = generation;
            touch(table, entry, now());
            if (!has_address(bucket, mac, port, NULL)) table->per_port[port]++;
            entry->next = bucket->head;
            bucket->head = entry;
            table->count++;
        }
    }
    check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
    return result;
}

void mac_forget_port(MacTable *table, unsigned port, uint64_t generation)
{
    check_thread(pthread_mutex_lock(&table->changes), "lock MAC changes");
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];
        check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
        MacEntry **link = &bucket->head;
        while (*link != NULL) {
            if ((*link)->port == port && (*link)->generation == generation)
                remove_entry(table, bucket, link);
            else link = &(*link)->next;
        }
        check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    }
    check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
}

void mac_clear(MacTable *table)
{
    check_thread(pthread_mutex_lock(&table->changes), "lock MAC changes");
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];
        check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
        while (bucket->head != NULL) {
            MacEntry *entry = bucket->head;
            bucket->head = entry->next;
            free(entry);
        }
        check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    }
    table->count = 0;
    memset(table->per_port, 0, table->num_ports * sizeof(*table->per_port));
    check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
}

int mac_snapshot(MacTable *table, MacRecord **records, size_t *count)
{
    *records = NULL;
    *count = 0;
    check_thread(pthread_mutex_lock(&table->changes), "lock MAC changes");
    if (table->count != 0) {
        *records = malloc(table->count * sizeof(**records));
        if (*records == NULL) {
            check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
            return -1;
        }
    }
    struct timespec sampled_at = now();
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];
        check_thread(pthread_mutex_lock(&bucket->lock), "lock MAC bucket");
        for (MacEntry *entry = bucket->head; entry != NULL; entry = entry->next) {
            if (expired(table, entry, sampled_at)) continue;
            MacRecord *record = &(*records)[(*count)++];
            memcpy(record->mac, entry->mac, MAC_SIZE);
            record->vlan = entry->vlan;
            record->port = entry->port;
        }
        check_thread(pthread_mutex_unlock(&bucket->lock), "unlock MAC bucket");
    }
    check_thread(pthread_mutex_unlock(&table->changes), "unlock MAC changes");
    return 0;
}

static void *age_entries(void *argument)
{
    MacTable *table = argument;
    check_thread(pthread_mutex_lock(&table->aging_lock), "lock aging state");
    while (!table->aging_stop) {
        struct timespec deadline = now();
        deadline.tv_sec++;
        int error = 0;
        while (!table->aging_stop && error != ETIMEDOUT) {
            error = pthread_cond_timedwait(&table->aging_wake, &table->aging_lock, &deadline);
            if (error != ETIMEDOUT) check_thread(error, "wait for aging timer");
        }
        if (table->aging_stop) break;
        check_thread(pthread_mutex_unlock(&table->aging_lock), "unlock aging state");
        mac_age(table);
        check_thread(pthread_mutex_lock(&table->aging_lock), "lock aging state");
    }
    check_thread(pthread_mutex_unlock(&table->aging_lock), "unlock aging state");
    return NULL;
}

int mac_table_init(MacTable *table, unsigned ports, size_t capacity,
                   unsigned limit, unsigned age_seconds)
{
    memset(table, 0, sizeof *table);
    table->capacity = capacity;
    table->num_ports = ports;
    table->per_port_limit = limit;
    table->age_seconds = age_seconds;
    atomic_init(&table->sequence, 0);
    table->per_port = calloc(ports, sizeof *table->per_port);
    if (table->per_port == NULL) return -1;
    unsigned initialized = 0;
    int error = pthread_mutex_init(&table->changes, NULL);
    if (error != 0) goto fail_counts;
    for (; initialized < MAC_BUCKETS; initialized++) {
        error = pthread_mutex_init(&table->buckets[initialized].lock, NULL);
        if (error != 0) goto fail_buckets;
    }
    error = pthread_mutex_init(&table->aging_lock, NULL);
    if (error != 0) goto fail_buckets;
    pthread_condattr_t attributes;
    error = pthread_condattr_init(&attributes);
    if (error != 0) goto fail_aging;
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0) error = pthread_cond_init(&table->aging_wake, &attributes);
    check_thread(pthread_condattr_destroy(&attributes), "destroy aging attributes");
    if (error == 0) return 0;
fail_aging:
    check_thread(pthread_mutex_destroy(&table->aging_lock), "destroy aging lock");
fail_buckets:
    while (initialized > 0)
        check_thread(pthread_mutex_destroy(&table->buckets[--initialized].lock), "destroy MAC bucket");
    check_thread(pthread_mutex_destroy(&table->changes), "destroy MAC changes");
fail_counts:
    free(table->per_port);
    errno = error;
    return -1;
}

int mac_start_aging(MacTable *table)
{
    int error = pthread_create(&table->aging_thread, NULL, age_entries, table);
    if (error != 0) {
        errno = error;
        return -1;
    }
    table->aging_started = 1;
    return 0;
}

void mac_stop_aging(MacTable *table)
{
    if (!table->aging_started) return;
    check_thread(pthread_mutex_lock(&table->aging_lock), "lock aging state");
    table->aging_stop = 1;
    check_thread(pthread_cond_signal(&table->aging_wake), "wake aging thread");
    check_thread(pthread_mutex_unlock(&table->aging_lock), "unlock aging state");
    check_thread(pthread_join(table->aging_thread, NULL), "join aging thread");
    table->aging_started = 0;
}

void mac_table_destroy(MacTable *table)
{
    mac_stop_aging(table);
    for (unsigned i = 0; i < MAC_BUCKETS; i++) {
        MacEntry *entry = table->buckets[i].head;
        while (entry != NULL) {
            MacEntry *next = entry->next;
            free(entry);
            entry = next;
        }
        check_thread(pthread_mutex_destroy(&table->buckets[i].lock), "destroy MAC bucket");
    }
    free(table->per_port);
    check_thread(pthread_mutex_destroy(&table->changes), "destroy MAC changes");
    check_thread(pthread_cond_destroy(&table->aging_wake), "destroy aging condition");
    check_thread(pthread_mutex_destroy(&table->aging_lock), "destroy aging lock");
}
