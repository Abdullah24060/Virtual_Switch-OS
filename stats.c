#define _POSIX_C_SOURCE 200809L
#include "stats.h"
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t stats_size(unsigned count)
{
    return sizeof(SharedStats) + count * sizeof(PortCounters);
}

static void lock_stats(SharedStats *stats)
{
    int error = pthread_mutex_lock(&stats->lock);
    if (error == EOWNERDEAD)
        error = pthread_mutex_consistent(&stats->lock);
    check_thread(error, "lock shared stats");
}

static int map_stats(int fd, size_t length, SharedStats **result)
{
    void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno;
    if (close(fd) == -1) {
        if (mapping != MAP_FAILED && munmap(mapping, length) == -1) perror("unmap stats");
        return -1;
    }
    if (mapping == MAP_FAILED) {
        errno = error;
        return -1;
    }
    *result = mapping;
    return 0;
}

int stats_create(const char *name, unsigned count, SharedStats **result)
{
    /* Switch already owns the TCP port. Discard a name left by a crashed instance. */
    if (shm_unlink(name) == -1 && errno != ENOENT) return -1;
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) return -1;
    size_t length = stats_size(count);
    if (ftruncate(fd, (off_t)length) == -1) {
        int error = errno;
        if (close(fd) == -1) perror("close stats");
        errno = error;
        goto fail_name;
    }
    SharedStats *stats;
    if (map_stats(fd, length, &stats) == -1) goto fail_name;

    pthread_mutexattr_t attr;
    int error = pthread_mutexattr_init(&attr);
    if (error == 0) {
        error = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (error == 0) error = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (error == 0) error = pthread_mutex_init(&stats->lock, &attr);
        check_thread(pthread_mutexattr_destroy(&attr), "destroy stats attributes");
    }
    if (error != 0) {
        if (munmap(stats, length) == -1) perror("unmap stats");
        errno = error;
        goto fail_name;
    }
    stats->count = count;
    stats->running = 1;
    *result = stats;
    return 0;

fail_name:
    error = errno;
    if (shm_unlink(name) == -1) perror("remove stats name");
    errno = error;
    return -1;
}

int stats_open(const char *name, SharedStats **result)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd == -1) return -1;
    struct stat info;
    int status = fstat(fd, &info);
    if (status == 0 && (info.st_size < (off_t)stats_size(1) ||
                        info.st_size > (off_t)stats_size(65535))) {
        status = -1;
        errno = EPROTO;
    }
    if (status == -1) {
        int error = errno;
        if (close(fd) == -1) perror("close stats");
        errno = error;
        return -1;
    }
    size_t length = (size_t)info.st_size;
    SharedStats *stats;
    if (map_stats(fd, length, &stats) == -1) return -1;
    if (stats->count == 0 || stats->count > 65535 || stats_size(stats->count) != length) {
        if (munmap(stats, length) == -1) perror("unmap stats");
        errno = EPROTO;
        return -1;
    }
    *result = stats;
    return 0;
}

int stats_close(SharedStats *stats)
{
    return munmap(stats, stats_size(stats->count));
}

int stats_stop(SharedStats *stats)
{
    lock_stats(stats);
    stats->running = 0;
    check_thread(pthread_mutex_unlock(&stats->lock), "unlock shared stats");
    /* Existing watchers can read the stopped flag through their mappings. */
    return stats_close(stats);
}

void stats_update(SharedStats *stats, unsigned port, PortCounters counters)
{
    if (stats == NULL) return;
    lock_stats(stats);
    stats->ports[port] = counters;
    check_thread(pthread_mutex_unlock(&stats->lock), "unlock shared stats");
}

int stats_snapshot(SharedStats *stats, PortCounters *counters)
{
    lock_stats(stats);
    int running = stats->running;
    memcpy(counters, stats->ports, stats->count * sizeof(*counters));
    check_thread(pthread_mutex_unlock(&stats->lock), "unlock shared stats");
    return running;
}
