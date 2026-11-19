#define _POSIX_C_SOURCE 200809L
#include "frame.h"
#include "port.h"
#include "admission.h"
#include "admin.h"
#include "options.h"
#include "config.h"
#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    sigset_t signals;
    int wake_fd;
    atomic_int stopping;
    atomic_uint pending;
} SignalContext;

enum { RELOAD_CONFIG = 1, DUMP_MAC = 2 };

static void *wait_for_signals(void *argument)
{
    SignalContext *context = argument;
    for (;;) {
        int signal_number;
        check_thread(sigwait(&context->signals, &signal_number), "sigwait");
        if (signal_number == SIGINT || signal_number == SIGTERM) {
            atomic_store(&context->stopping, 1);
            notify_main(context->wake_fd, 'S');
            break;
        }
        atomic_fetch_or(&context->pending, signal_number == SIGHUP ? RELOAD_CONFIG : DUMP_MAC);
        notify_main(context->wake_fd, 'C');
    }
    return NULL;
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int drain_wake_pipe(int fd)
{
    char events[128];
    for (;;) {
        ssize_t n = read(fd, events, sizeof events);
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return 0;
    }
}

static void reload_config(Admin *admin, const SwitchOptions *baseline, Logger *logger)
{
    if (baseline->config_path == NULL) {
        logger_event(logger, 1, "config reload skipped: start with --config to enable reload");
        return;
    }
    SwitchOptions config;
    unsigned line;
    if (config_load(baseline, &config, &line) == -1) {
        if (line != 0)
            logger_event(logger, 1, "config reload rejected: %s line %u", baseline->config_path, line);
        else
            logger_event(logger, 1, "config reload rejected: %s: %s",
                         baseline->config_path, strerror(errno));
        return;
    }
    admin_apply_config(admin, &config);
    logger_event(logger, 1, "config reloaded from %s (send_rate=%u)",
                 baseline->config_path, config.send_rate);
    free(config.vlans);
}

static int run_switch(Admin *admin, int wake_fd, SignalContext *signals,
                      const SwitchOptions *options, Logger *logger)
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
        if (drain_wake_pipe(wake_fd) == -1) {
            perror("read events");
            return 1;
        }
        if (admission_reap(admin->admission) == -1) return 1;
        if (atomic_load(&signals->stopping)) return 0;
        unsigned pending = atomic_exchange(&signals->pending, 0);
        if (pending & RELOAD_CONFIG) reload_config(admin, options, logger);
        if (atomic_load(&signals->stopping)) return 0;
        if (pending & DUMP_MAC) {
            size_t count;
            if (mac_dump_file(admin->mac, options->dump_path, &count) == -1)
                logger_event(logger, 1, "MAC dump failed: %s: %s", options->dump_path, strerror(errno));
            else
                logger_event(logger, 1, "MAC table dumped to %s (%zu entries)", options->dump_path, count);
        }
    }
}

int main(int argc, char *argv[])
{
    SwitchOptions options, initial;
    int listener = -1, wake[2] = {-1, -1};
    int reuse = 1, status = 1, signal_started = 0;
    unsigned initialized = 0;
    Port *ports = NULL;
    MacTable table;
    Admission admission;
    Admin admin;
    AdminNames names;
    SharedStats *stats = NULL;
    Logger logger = {0};
    int logger_started = 0;
    RateTimer rate_timer;
    int mac_ready = 0, admission_ready = 0, timer_started = 0, admin_started = 0;
    pthread_t signal_thread;
    SignalContext signals;
    struct sockaddr_in address = {0};

    if (parse_options(argc, argv, &options) == -1) {
        if (errno == EINVAL) print_switch_usage();
        else perror("parse switch options");
        free(options.vlans);
        return 1;
    }
    unsigned line;
    if (config_load(&options, &initial, &line) == -1) {
        if (line != 0) fprintf(stderr, "invalid config %s line %u\n", options.config_path, line);
        else perror("load switch config");
        free(options.vlans);
        return 1;
    }
    unsigned tcp_port = options.tcp_port, num_ports = options.num_ports;
    unsigned send_rate = initial.send_rate;
    atomic_init(&signals.stopping, 0);
    atomic_init(&signals.pending, 0);
    if (ignore_sigpipe() == -1 || sigemptyset(&signals.signals) == -1 ||
        sigaddset(&signals.signals, SIGINT) == -1 ||
        sigaddset(&signals.signals, SIGTERM) == -1 ||
        sigaddset(&signals.signals, SIGHUP) == -1 ||
        sigaddset(&signals.signals, SIGUSR1) == -1) {
        perror("set up signals");
        goto cleanup;
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
    if (logger_start(&logger, options.log_path) == -1) {
        perror("start switch logger");
        goto cleanup;
    }
    logger_started = 1;
    if (mac_table_init(&table, num_ports, options.capacity,
                       options.mac_limit, options.age_seconds) == -1) {
        perror("initialize MAC table");
        goto cleanup;
    }
    mac_ready = 1;
    if (admin_names(&names, tcp_port) == -1 || stats_create(names.stats, num_ports, &stats) == -1) {
        perror("initialize shared stats");
        goto cleanup;
    }
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
        ports[initialized].stats = stats;
        ports[initialized].logger = &logger;
        ports[initialized].rate_timer = &rate_timer;
        ports[initialized].send_interval_ns = (NS_PER_SECOND + send_rate - 1) / send_rate;
        if (initial.vlans[initialized] != 0)
            ports[initialized].vlan = initial.vlans[initialized];
        ports[initialized].trunk = initialized + 1 == initial.trunk;
        ports[initialized].sniffer = initialized + 1 == initial.sniffer;
    }
    if (initial.mirror_source != 0)
        ports[initial.mirror_source - 1].mirror_to = &ports[initial.sniffer - 1];
    if (admission_init(&admission, ports, num_ports, listener, wake[1], options.wait_seconds,
                       &signals.stopping) == -1) {
        perror("initialize admission");
        goto cleanup;
    }
    admission_ready = 1;
    if (mac_start_aging(&table) == -1) {
        perror("start aging thread");
        goto cleanup;
    }
    if (rate_timer_start(&rate_timer, ports, num_ports) == -1) {
        perror("start rate timer");
        goto cleanup;
    }
    timer_started = 1;
    signals.wake_fd = wake[1];
    int error = pthread_create(&signal_thread, NULL, wait_for_signals, &signals);
    if (error != 0) {
        fprintf(stderr, "create signal thread: %s\n", strerror(error));
        goto cleanup;
    }
    signal_started = 1;
    if (admin_start(&admin, &names, &admission, &table, &logger) == -1) {
        perror("start admin socket");
        goto cleanup;
    }
    admin_started = 1;
    if (admission_start(&admission) == -1) {
        perror("start admission thread");
        goto cleanup;
    }
    logger_event(&logger, 1, "listening tcp_port=%u num_ports=%u queue_capacity_per_class=%d send_rate=%u",
                 tcp_port, num_ports, QUEUE_CAPACITY, send_rate);
    if (options.config_path != NULL)
        logger_event(&logger, 0, "config loaded from %s", options.config_path);
    status = run_switch(&admin, wake[0], &signals, &options, &logger);

cleanup:
    free(initial.vlans);
    free(options.vlans);
    atomic_store(&signals.stopping, 1);
    if (logger_started) logger_event(&logger, 0, "switch shutting down");
    if (admin_started) admin_stop(&admin);
    /* Remove the name before another instance can acquire our TCP port. */
    if (stats != NULL && shm_unlink(names.stats) == -1) {
        perror("remove shared stats name");
        status = 1;
    }
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
    if (timer_started) rate_timer_stop(&rate_timer);
    /* Receivers and the timer can inspect every port. Join them before destroying locks. */
    for (unsigned i = 0; i < initialized; i++) port_destroy(&ports[i]);
    free(ports);
    if (admission_ready) admission_destroy(&admission);
    if (mac_ready) mac_table_destroy(&table);
    if (stats != NULL && stats_stop(stats) == -1) {
        perror("stop shared stats");
        status = 1;
    }
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
    if (logger_started) {
        logger_event(&logger, 0, "switch stopped");
        if (logger_stop(&logger) == -1) status = 1;
    }
    return status;
}
