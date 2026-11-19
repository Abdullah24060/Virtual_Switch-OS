#define _POSIX_C_SOURCE 200809L
#include "admin.h"
#include "sync.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    const char *state, *role;
    unsigned vlan, mirror;
    unsigned rate;
} PortInfo;

static int reply(int fd, const char *message)
{
    return dprintf(fd, "%s\n", message) < 0 ? -1 : 0;
}

static int show_ports(Admin *admin, int fd)
{
    Admission *a = admin->admission;
    PortInfo *info = calloc(a->count, sizeof *info);
    if (info == NULL) return reply(fd, "ERR cannot allocate port snapshot");
    check_thread(pthread_rwlock_rdlock(&a->forwarding), "lock forwarding");
    for (unsigned i = 0; i < a->count; i++) {
        Port *p = &a->ports[i];
        check_thread(pthread_mutex_lock(&p->lock), "lock port");
        info[i].state = !p->enabled ? "down" :
                        p->state == PORT_ACTIVE ? "active" :
                        p->state == PORT_CLOSING ? "closing" : "free";
        info[i].role = p->trunk ? "trunk" : p->sniffer ? "sniffer" : "access";
        info[i].vlan = p->trunk || p->sniffer ? 0 : p->vlan;
        info[i].mirror = p->mirror_to == NULL ? 0 : (unsigned)(p->mirror_to - a->ports) + 1;
        info[i].rate = (unsigned)((NS_PER_SECOND + p->send_interval_ns / 2) / p->send_interval_ns);
        check_thread(pthread_mutex_unlock(&p->lock), "unlock port");
    }
    check_thread(pthread_rwlock_unlock(&a->forwarding), "unlock forwarding");
    int status = reply(fd, "OK");
    for (unsigned i = 0; status == 0 && i < a->count; i++) {
        PortInfo row = info[i];
        if (dprintf(fd, "port %u state=%s role=%s vlan=%u mirror=%u rate=%u\n", i + 1,
                    row.state, row.role, row.vlan, row.mirror, row.rate) < 0)
            status = -1;
    }
    free(info);
    return status;
}

static int show_mac(Admin *admin, int fd)
{
    MacRecord *records;
    size_t count;
    check_thread(pthread_rwlock_rdlock(&admin->admission->forwarding), "lock forwarding");
    int snapshot = mac_snapshot(admin->mac, &records, &count);
    check_thread(pthread_rwlock_unlock(&admin->admission->forwarding), "unlock forwarding");
    if (snapshot == -1)
        return reply(fd, "ERR cannot allocate MAC snapshot");
    int status = dprintf(fd, "OK\nMAC table (%zu entries)\n", count) < 0 ? -1 : 0;
    for (size_t i = 0; status == 0 && i < count; i++) {
        MacRecord *row = &records[i];
        if (dprintf(fd, "%02x:%02x:%02x:%02x:%02x:%02x vlan=%u port=%u\n",
                    row->mac[0], row->mac[1], row->mac[2], row->mac[3], row->mac[4],
                    row->mac[5], (unsigned)row->vlan, row->port + 1) < 0)
            status = -1;
    }
    free(records);
    return status;
}

static int set_vlan(Admin *admin, unsigned index, uint16_t vlan)
{
    Admission *a = admin->admission;
    Port *port = &a->ports[index];
    check_thread(pthread_rwlock_wrlock(&a->forwarding), "lock forwarding for VLAN");
    check_thread(pthread_mutex_lock(&port->lock), "lock port");
    int supported = !port->trunk && !port->sniffer;
    int changed = supported && port->vlan != vlan;
    uint64_t generation = port->generation;
    if (changed) {
        port->vlan = vlan;
        port_discard_output(port);
    }
    check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
    if (changed) mac_forget_port(admin->mac, index, generation);
    check_thread(pthread_rwlock_unlock(&a->forwarding), "unlock forwarding");
    return supported;
}

/* The forwarding writer excludes receivers while each role changes in turn. */
static void set_sniffer(Admin *admin, Port *port, int enabled)
{
    check_thread(pthread_mutex_lock(&port->lock), "lock sniffer");
    port->sniffer = enabled;
    port_discard_output(port);
    uint64_t generation = port->generation;
    check_thread(pthread_mutex_unlock(&port->lock), "unlock sniffer");
    if (enabled)
        mac_forget_port(admin->mac, (unsigned)(port - admin->admission->ports), generation);
}

static void set_mirror_target(Port *source, Port *target)
{
    check_thread(pthread_mutex_lock(&source->lock), "lock mirror source");
    source->mirror_to = target;
    check_thread(pthread_mutex_unlock(&source->lock), "unlock mirror source");
}

static int set_mirror(Admin *admin, unsigned from, unsigned to)
{
    Admission *a = admin->admission;
    Port *source = &a->ports[from], *target = &a->ports[to];
    if (from == to) return 0;
    check_thread(pthread_rwlock_wrlock(&a->forwarding), "lock forwarding for mirror");
    if (target->trunk) {
        check_thread(pthread_rwlock_unlock(&a->forwarding), "unlock forwarding");
        return 0;
    }
    if (admin->mirror_source != NULL && admin->mirror_source != source)
        set_mirror_target(admin->mirror_source, NULL);
    if (admin->sniffer != target) {
        if (admin->sniffer != NULL) set_sniffer(admin, admin->sniffer, 0);
        set_sniffer(admin, target, 1);
    } else if (admin->mirror_source != source) {
        check_thread(pthread_mutex_lock(&target->lock), "lock sniffer");
        port_discard_output(target);
        check_thread(pthread_mutex_unlock(&target->lock), "unlock sniffer");
    }
    set_mirror_target(source, target);
    admin->mirror_source = source;
    admin->sniffer = target;
    check_thread(pthread_rwlock_unlock(&a->forwarding), "unlock forwarding");
    return 1;
}

void admin_apply_config(Admin *admin, const SwitchOptions *config)
{
    Admission *a = admin->admission;
    Port *source = config->mirror_source != 0 ? &a->ports[config->mirror_source - 1] : NULL;
    Port *sniffer = config->sniffer != 0 ? &a->ports[config->sniffer - 1] : NULL;
    uint64_t interval = (NS_PER_SECOND + config->send_rate - 1) / config->send_rate;
    int rate_changed = 0;
    check_thread(pthread_rwlock_wrlock(&a->forwarding), "lock forwarding for reload");
    for (unsigned i = 0; i < a->count; i++) {
        Port *port = &a->ports[i];
        uint16_t vlan = config->vlans[i] != 0 ? config->vlans[i] : 1;
        int trunk = i + 1 == config->trunk;
        int is_sniffer = port == sniffer;
        check_thread(pthread_mutex_lock(&port->lock), "lock port");
        int role_changed = port->vlan != vlan || port->trunk != trunk || port->sniffer != is_sniffer;
        int forget = role_changed && port->state != PORT_FREE;
        uint64_t generation = port->generation;
        port->vlan = vlan;
        port->trunk = trunk;
        port->sniffer = is_sniffer;
        port->mirror_to = port == source ? sniffer : NULL;
        if (role_changed || (is_sniffer && admin->mirror_source != source))
            port_discard_output(port);
        if (port->send_interval_ns != interval) {
            if (port->refill_at != 0 && port->state == PORT_ACTIVE) {
                port->refill_at = port->refill_at - port->send_interval_ns + interval;
                port->send_token = monotonic_ns() >= port->refill_at;
                check_thread(pthread_cond_signal(&port->not_empty), "wake sender after rate change");
            }
            port->send_interval_ns = interval;
            rate_changed = 1;
        }
        check_thread(pthread_mutex_unlock(&port->lock), "unlock port");
        if (forget) mac_forget_port(admin->mac, i, generation);
    }
    admin->mirror_source = source;
    admin->sniffer = sniffer;
    check_thread(pthread_rwlock_unlock(&a->forwarding), "unlock forwarding");
    if (rate_changed) rate_timer_notify(a->ports[0].rate_timer);
}

static int command(Admin *admin, int fd, char *line)
{
    char *words[4], *save;
    unsigned n = 0, port, value;
    for (char *word = strtok_r(line, " \t", &save); word != NULL;
         word = strtok_r(NULL, " \t", &save)) {
        if (n == 4) return reply(fd, "ERR invalid command");
        words[n++] = word;
    }
    if (n == 2 && strcmp(words[0], "show") == 0) {
        if (strcmp(words[1], "ports") == 0) return show_ports(admin, fd);
        if (strcmp(words[1], "mac") == 0) return show_mac(admin, fd);
    }
    if (n == 1 && strcmp(words[0], "stats") == 0)
        return dprintf(fd, "OK\n%s\n", admin->names.stats) < 0 ? -1 : 0;
    if (n == 2 && strcmp(words[0], "clear") == 0 && strcmp(words[1], "mac") == 0) {
        check_thread(pthread_rwlock_wrlock(&admin->admission->forwarding), "lock forwarding for clear");
        mac_clear(admin->mac);
        check_thread(pthread_rwlock_unlock(&admin->admission->forwarding), "unlock forwarding");
        logger_event(admin->logger, 0, "admin cleared MAC table");
        return dprintf(fd, "OK\nMAC table cleared\n") < 0 ? -1 : 0;
    }
    if (n >= 2 && parse_number(words[1], 1, admin->admission->count, &port) == 0) {
        if (n == 2 && strcmp(words[0], "shutdown") == 0) {
            if (admission_shutdown_port(admin->admission, port - 1) == -1)
                return reply(fd, "ERR could not shut down port");
            logger_event(admin->logger, 0, "admin shut down port %u", port);
            return dprintf(fd, "OK\nport %u down\n", port) < 0 ? -1 : 0;
        }
        if (n == 2 && strcmp(words[0], "up") == 0) {
            if (admission_up_port(admin->admission, port - 1) == -1)
                return reply(fd, "ERR could not enable port");
            logger_event(admin->logger, 0, "admin enabled port %u", port);
            return dprintf(fd, "OK\nport %u enabled\n", port) < 0 ? -1 : 0;
        }
        if (n == 3 && strcmp(words[0], "vlan") == 0 && parse_number(words[2], 1, 4094, &value) == 0) {
            if (!set_vlan(admin, port - 1, (uint16_t)value))
                return reply(fd, "ERR VLAN changes require an access port");
            logger_event(admin->logger, 0, "admin set port %u VLAN to %u", port, value);
            return dprintf(fd, "OK\nport %u vlan=%u\n", port, value) < 0 ? -1 : 0;
        }
        if (n == 3 && strcmp(words[0], "mirror") == 0 &&
            parse_number(words[2], 1, admin->admission->count, &value) == 0) {
            if (!set_mirror(admin, port - 1, value - 1))
                return reply(fd, "ERR use a different non-trunk port as the sniffer");
            logger_event(admin->logger, 0, "admin set port %u to mirror to port %u", port, value);
            return dprintf(fd, "OK\nport %u mirrors to port %u\n", port, value) < 0 ? -1 : 0;
        }
    }
    return reply(fd, "ERR invalid command or port");
}

static int read_command(int fd, char line[ADMIN_REQUEST_SIZE])
{
    uint64_t deadline = monotonic_ns() + 2 * NS_PER_SECOND;
    size_t used = 0;
    while (used < ADMIN_REQUEST_SIZE - 1) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) return 0;
        int ms = (int)((deadline - now + 999999) / 1000000);
        struct pollfd request = {.fd = fd, .events = POLLIN};
        int ready = poll(&request, 1, ms);
        if (ready == -1 && errno == EINTR) continue;
        if (ready <= 0) return ready;
        ssize_t n = read(fd, line + used, ADMIN_REQUEST_SIZE - 1 - used);
        if (n == -1 && errno == EINTR) continue;
        if (n <= 0) return (int)n;
        for (size_t i = used; i < used + (size_t)n; i++) {
            if (line[i] == '\n') {
                line[i] = '\0';
                return 1;
            }
            if ((unsigned char)line[i] < ' ' && line[i] != '\t') return 0;
        }
        used += (size_t)n;
    }
    return 0;
}

static void close_admin_fd(int fd)
{
    if (close(fd) == -1) perror("close admin socket or pipe");
}

static void *serve_admin(void *argument)
{
    Admin *admin = argument;
    struct pollfd fds[2] = {{.fd = admin->wake[0], .events = POLLIN},
                           {.fd = admin->listener, .events = POLLIN}};
    for (;;) {
        if (poll(fds, 2, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll admin socket");
            break;
        }
        if (fds[0].revents != 0) break;
        if (!(fds[1].revents & POLLIN)) {
            fprintf(stderr, "admin listener failed\n");
            break;
        }
        int client = accept(admin->listener, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept admin client");
            break;
        }
        check_thread(pthread_mutex_lock(&admin->client_lock), "lock admin client");
        int stopping = admin->stopping;
        if (!stopping) admin->client = client;
        check_thread(pthread_mutex_unlock(&admin->client_lock), "unlock admin client");
        if (!stopping) {
            struct timeval timeout = {.tv_sec = 2};
            char line[ADMIN_REQUEST_SIZE];
            if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == -1) {
                perror("set admin send timeout");
            } else {
                int read = read_command(client, line);
                int status = read == 1 ? command(admin, client, line) :
                             read == 0 ? reply(client, "ERR incomplete or invalid request") : -1;
                if (status == -1 && errno != EPIPE && errno != ECONNRESET &&
                    errno != EAGAIN && errno != EWOULDBLOCK)
                    perror("admin client");
            }
        }
        check_thread(pthread_mutex_lock(&admin->client_lock), "lock admin client");
        admin->client = -1;
        close_admin_fd(client);
        check_thread(pthread_mutex_unlock(&admin->client_lock), "unlock admin client");
        if (stopping) break;
    }
    return NULL;
}

static int prepare_path(const AdminNames *names)
{
    struct stat info;
    if (mkdir(names->directory, 0700) == -1 && errno != EEXIST) return -1;
    if (lstat(names->directory, &info) == -1) return -1;
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0) {
        errno = EACCES;
        return -1;
    }
    const char *path = names->address.sun_path;
    if (lstat(path, &info) == -1) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid()) {
        errno = EEXIST;
        return -1;
    }
    /* TCP already belongs to this instance; this socket pathname is stale. */
    return unlink(path);
}

int admin_start(Admin *admin, const AdminNames *names, Admission *admission, MacTable *mac,
                Logger *logger)
{
    *admin = (Admin){.names = *names, .admission = admission, .mac = mac, .logger = logger, .client = -1};
    for (unsigned i = 0; i < admission->count; i++) {
        if (admission->ports[i].mirror_to != NULL) {
            admin->mirror_source = &admission->ports[i];
            admin->sniffer = admission->ports[i].mirror_to;
        }
    }
    if (prepare_path(names) == -1) return -1;
    admin->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (admin->listener == -1) return -1;
    int error;
    if (bind(admin->listener, (const struct sockaddr *)&names->address, sizeof names->address) == -1)
        goto fail_listener;
    if (listen(admin->listener, 8) == -1 || pipe(admin->wake) == -1) goto fail_path;
    error = pthread_mutex_init(&admin->client_lock, NULL);
    if (error == 0) {
        error = pthread_create(&admin->thread, NULL, serve_admin, admin);
        if (error == 0) return 0;
        check_thread(pthread_mutex_destroy(&admin->client_lock), "destroy admin mutex");
    }
    close_admin_fd(admin->wake[0]);
    close_admin_fd(admin->wake[1]);
    errno = error;
fail_path:
    error = errno;
    if (unlink(names->address.sun_path) == -1) perror("remove admin socket");
    errno = error;
fail_listener:
    error = errno;
    close_admin_fd(admin->listener);
    errno = error;
    return -1;
}

void admin_stop(Admin *admin)
{
    check_thread(pthread_mutex_lock(&admin->client_lock), "lock admin client");
    admin->stopping = 1;
    if (admin->client != -1 && shutdown(admin->client, SHUT_RDWR) == -1 && errno != ENOTCONN)
        perror("shutdown admin client");
    check_thread(pthread_mutex_unlock(&admin->client_lock), "unlock admin client");
    notify_main(admin->wake[1], 'S');
    check_thread(pthread_join(admin->thread, NULL), "join admin thread");
    check_thread(pthread_mutex_destroy(&admin->client_lock), "destroy admin mutex");
    close_admin_fd(admin->wake[0]);
    close_admin_fd(admin->wake[1]);
    close_admin_fd(admin->listener);
    if (unlink(admin->names.address.sun_path) == -1) perror("remove admin socket");
}
