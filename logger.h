#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define LOG_CAPACITY 512
#define LOG_TEXT_SIZE 256

typedef struct {
    struct timespec time;
    int echo;
    char text[LOG_TEXT_SIZE];
} LogEntry;

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    FILE *file;
    int console, stopping, failed;
    size_t head, count, dropped;
    LogEntry entries[LOG_CAPACITY];
} Logger;

int logger_start(Logger *logger, const char *path);
void logger_event(Logger *logger, int echo, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int logger_stop(Logger *logger);

#endif
