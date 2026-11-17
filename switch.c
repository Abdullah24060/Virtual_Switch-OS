#define _POSIX_C_SOURCE 200809L
#include "frame.h"
#include "port.h"
#include "admission.h"
#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    sigset_t signals;
    int wake_fd;
    atomic_int stopping;
} SignalContext;

static void *wait_for_shutdown(void *argument)
{
    SignalContext *context = argument;
    int signal_number;
    check_thread(sigwait(&context->signals, &signal_number), "sigwait");
    atomic_store(&context->stopping, 1);
    notify_main(context->wake_fd, 'S');
    return NULL;
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int read_events(int fd)
{
    char events[128];
    int stopping = 0;
    for (;;) {
        ssize_t n = read(fd, events, sizeof events);
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (events[i] == 'S') stopping = 1;
        }
    }
    return stopping;
}

static int run_switch(Admission *admission, int wake_fd, const atomic_int *stopping)
{
    struct pollfd fd = { .fd = wake_fd, .events = POLLIN };
    for (;;) {
        if (poll(&fd, 1, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "notification pipe failed\n");
            return 1;
        }
        if (read_events(wake_fd) == -1) {
            perror("read events");
            return 1;
        }
        if (admission_reap(admission) == -1) return 1;
        if (atomic_load(stopping)) return 0;
    }
}

int main(int argc, char *argv[])
{
    unsigned tcp_port, num_ports;
    unsigned capacity = 1024, mac_limit = 8, age_seconds = 60, wait_seconds = 5;
    int listener = -1, wake[2] = {-1, -1};
    int reuse = 1, status = 1, signal_started = 0;
    unsigned initialized = 0;
    Port *ports = NULL;
    MacTable table;
    Admission admission;
    int mac_ready = 0, admission_ready = 0;
    pthread_t signal_thread;
    SignalContext signals;
    struct sockaddr_in address = {0};

    if ((argc != 3 && argc != 7) || parse_number(argv[1], 1, 65535, &tcp_port) == -1 ||
        parse_number(argv[2], 1, 65535, &num_ports) == -1 ||
        (argc == 7 && (parse_number(argv[3], 1, 1000000, &capacity) == -1 ||
                       parse_number(argv[4], 1, 65535, &mac_limit) == -1 ||
                       parse_number(argv[5], 1, 86400, &age_seconds) == -1 ||
                       parse_number(argv[6], 1, 3600, &wait_seconds) == -1))) {
        fprintf(stderr, "usage: ./switch <tcp_port> <num_ports> [mac_capacity macs_per_port age_seconds wait_seconds]\n");
        return 1;
    }
    atomic_init(&signals.stopping, 0);
    if (ignore_sigpipe() == -1 || sigemptyset(&signals.signals) == -1 ||
        sigaddset(&signals.signals, SIGINT) == -1 ||
        sigaddset(&signals.signals, SIGTERM) == -1) {
        perror("set up signals");
        return 1;
    }
    check_thread(pthread_sigmask(SIG_BLOCK, &signals.signals, NULL), "block signals");
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        goto cleanup;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)tcp_port);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof address) == -1 ||
        listen(listener, (int)num_ports) == -1) {
        perror("set up listener");
        goto cleanup;
    }
    if (pipe(wake) == -1 || nonblocking(wake[0]) == -1 || nonblocking(wake[1]) == -1) {
        perror("set up notification pipe");
        goto cleanup;
    }
    if (mac_table_init(&table, num_ports, capacity, mac_limit, age_seconds) == -1) {
        perror("initialize MAC table");
        goto cleanup;
    }
    mac_ready = 1;
    ports = calloc(num_ports, sizeof *ports);
    if (ports == NULL) {
        perror("allocate ports");
        goto cleanup;
    }
    for (; initialized < num_ports; initialized++) {
        if (port_init(&ports[initialized], ports, num_ports, wake[1]) == -1) {
            perror("initialize port");
            goto cleanup;
        }
        ports[initialized].mac_table = &table;
    }
    if (admission_init(&admission, ports, num_ports, listener, wake[1], wait_seconds,
                       &signals.stopping) == -1) {
        perror("initialize admission");
        goto cleanup;
    }
    admission_ready = 1;
    if (mac_start_aging(&table) == -1) {
        perror("start aging thread");
        goto cleanup;
    }
    signals.wake_fd = wake[1];
    int error = pthread_create(&signal_thread, NULL, wait_for_shutdown, &signals);
    if (error != 0) {
        fprintf(stderr, "create signal thread: %s\n", strerror(error));
        goto cleanup;
    }
    signal_started = 1;
    if (admission_start(&admission) == -1) {
        perror("start admission thread");
        goto cleanup;
    }
    if (printf("listening tcp_port=%u num_ports=%u queue_capacity=%d\n",
               tcp_port, num_ports, QUEUE_CAPACITY) < 0 || fflush(stdout) == EOF) {
        perror("print startup");
        goto cleanup;
    }
    status = run_switch(&admission, wake[0], &signals.stopping);

cleanup:
    atomic_store(&signals.stopping, 1);
    if (admission_ready) {
        admission_stop(&admission);
        if (admission.status != 0) status = 1;
    }
    if (mac_ready) mac_stop_aging(&table);
    if (listener != -1 && close(listener) == -1) {
        perror("close listener");
        status = 1;
    }
    for (unsigned i = 0; i < initialized; i++) port_stop(&ports[i]);
    if (admission_ready) {
        if (admission_reap(&admission) == -1) status = 1;
    } else {
        for (unsigned i = 0; i < initialized; i++) {
            if (port_reap(&ports[i], i + 1) == -1) status = 1;
        }
    }
    /* Receivers can inspect every port: join ALL before destroying ANY lock. */
    for (unsigned i = 0; i < initialized; i++) port_destroy(&ports[i]);
    free(ports);
    if (admission_ready) admission_destroy(&admission);
    if (mac_ready) mac_table_destroy(&table);
    if (signal_started) {
        /* Also wake sigwait if shutdown followed an error instead of a signal. */
        int error = pthread_kill(signal_thread, SIGTERM);
        if (error != ESRCH) check_thread(error, "wake signal thread");
        check_thread(pthread_join(signal_thread, NULL), "join signal thread");
    }
    for (unsigned i = 0; i < 2; i++) {
        if (wake[i] != -1 && close(wake[i]) == -1) {
            perror("close notification pipe");
            status = 1;
        }
    }
    return status;
}
