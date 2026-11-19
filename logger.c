#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_entry(Logger *logger, const LogEntry *entry)
{
    if (!logger->failed) {
        struct tm utc;
        char date[32];
        if (gmtime_r(&entry->time.tv_sec, &utc) == NULL ||
            strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &utc) == 0)
            strcpy(date, "time unavailable");
        if (fprintf(logger->file, "%s.%03ldZ %s\n", date,
                    entry->time.tv_nsec / 1000000, entry->text) < 0 ||
            fflush(logger->file) == EOF) {
            perror("write switch log");
            logger->failed = 1;
        }
    }
    if (entry->echo && logger->console != -1) {
        char line[LOG_TEXT_SIZE + 1];
        int size = snprintf(line, sizeof line, "%s\n", entry->text);
        if (size > 0 && (size_t)size < sizeof line) {
            ssize_t result;
            do {
                result = write(logger->console, line, (size_t)size);
            } while (result == -1 && errno == EINTR);
            if (result == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (close(logger->console) == -1) perror("close log console");
                logger->console = -1;
            }
        }
    }
}

static void *write_log(void *argument)
{
    Logger *logger = argument;
    for (;;) {
        LogEntry entry = {0};
        size_t dropped = 0;
        check_thread(pthread_mutex_lock(&logger->lock), "lock logger");
        while (logger->count == 0 && logger->dropped == 0 && !logger->stopping)
            check_thread(pthread_cond_wait(&logger->ready, &logger->lock), "wait for log entry");
        if (logger->count != 0) {
            entry = logger->entries[logger->head];
            logger->head = (logger->head + 1) % LOG_CAPACITY;
            logger->count--;
        } else if (logger->dropped != 0) {
            dropped = logger->dropped;
            logger->dropped = 0;
        } else {
            check_thread(pthread_mutex_unlock(&logger->lock), "unlock logger");
            break;
        }
        check_thread(pthread_mutex_unlock(&logger->lock), "unlock logger");
        if (dropped != 0) {
            if (clock_gettime(CLOCK_REALTIME, &entry.time) == -1) perror("timestamp log entry");
            if (snprintf(entry.text, sizeof entry.text, "log queue full: dropped %zu events", dropped) < 0)
                continue;
        }
        write_entry(logger, &entry);
    }
    return NULL;
}

int logger_start(Logger *logger, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK | O_CLOEXEC, 0644);
    if (fd == -1) return -1;
    struct stat info;
    int result = fstat(fd, &info);
    if (result == 0 && !S_ISREG(info.st_mode)) {
        errno = EINVAL;
        result = -1;
    }
    if (result == 0) logger->file = fdopen(fd, "a");
    if (result == -1 || logger->file == NULL) {
        int error = errno;
        if (close(fd) == -1) perror("close log file");
        errno = error;
        return -1;
    }
    /* A separate nonblocking descriptor keeps a full console pipe out of shutdown. */
    logger->console = open("/proc/self/fd/1", O_WRONLY | O_APPEND | O_NONBLOCK | O_CLOEXEC);
    int error = pthread_mutex_init(&logger->lock, NULL);
    if (error == 0) {
        error = pthread_cond_init(&logger->ready, NULL);
        if (error == 0) {
            error = pthread_create(&logger->thread, NULL, write_log, logger);
            if (error == 0) return 0;
            check_thread(pthread_cond_destroy(&logger->ready), "destroy log condition");
        }
        check_thread(pthread_mutex_destroy(&logger->lock), "destroy log lock");
    }
    if (logger->console != -1 && close(logger->console) == -1) perror("close log console");
    if (fclose(logger->file) == EOF) perror("close log file");
    errno = error;
    return -1;
}

void logger_event(Logger *logger, int echo, const char *format, ...)
{
    if (logger == NULL) return;
    int saved = errno;
    LogEntry entry = {.echo = echo};
    if (clock_gettime(CLOCK_REALTIME, &entry.time) == -1) entry.time = (struct timespec){0};
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(entry.text, sizeof entry.text, format, arguments);
    va_end(arguments);
    if (size >= 0) {
        check_thread(pthread_mutex_lock(&logger->lock), "lock logger");
        if (logger->count == LOG_CAPACITY) {
            logger->dropped++;
        } else {
            size_t tail = (logger->head + logger->count) % LOG_CAPACITY;
            logger->entries[tail] = entry;
            logger->count++;
        }
        check_thread(pthread_cond_signal(&logger->ready), "wake logger");
        check_thread(pthread_mutex_unlock(&logger->lock), "unlock logger");
    }
    errno = saved;
}

int logger_stop(Logger *logger)
{
    check_thread(pthread_mutex_lock(&logger->lock), "lock logger");
    logger->stopping = 1;
    check_thread(pthread_cond_signal(&logger->ready), "wake logger");
    check_thread(pthread_mutex_unlock(&logger->lock), "unlock logger");
    check_thread(pthread_join(logger->thread, NULL), "join logger");
    check_thread(pthread_cond_destroy(&logger->ready), "destroy log condition");
    check_thread(pthread_mutex_destroy(&logger->lock), "destroy log lock");
    int result = fclose(logger->file);
    if (logger->console != -1 && close(logger->console) == -1) perror("close log console");
    return result == EOF || logger->failed ? -1 : 0;
}
