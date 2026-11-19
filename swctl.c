#define _POSIX_C_SOURCE 200809L
#include "admin_protocol.h"
#include "frame.h"
#include "stats.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int show_stats(const char *name, int watch)
{
    SharedStats *stats;
    if (stats_open(name, &stats) == -1) {
        perror("open switch stats");
        return 1;
    }
    unsigned count = stats->count;
    PortCounters *values = calloc(count, sizeof *values);
    int status = 0;
    if (values == NULL) {
        perror("allocate stats snapshot");
        status = 1;
        goto done;
    }
    unsigned sample = 1;
    do {
        if (!stats_snapshot(stats, values)) {
            fprintf(stderr, "switch stopped\n");
            break;
        }
        if (watch && printf("sample %u\n", sample++) < 0) status = 1;
        for (unsigned i = 0; i < count && status == 0; i++) {
            const PortCounters *p = &values[i];
            if (printf("port %u in=%" PRIu64 " out=%" PRIu64 " dropped=%" PRIu64
                       " mirrored=%" PRIu64 "\n", i + 1, p->in, p->out,
                       p->dropped, p->mirrored) < 0)
                status = 1;
        }
        if (fflush(stdout) == EOF) status = 1;
        if (watch && status == 0) {
            struct timespec delay = {.tv_sec = 1};
            while (nanosleep(&delay, &delay) == -1) {
                if (errno == EINTR) continue;
                perror("wait for next stats sample");
                status = 1;
                break;
            }
        }
    } while (watch && status == 0);
    free(values);
done:
    if (stats_close(stats) == -1) {
        perror("unmap stats");
        status = 1;
    }
    return status;
}

static int read_reply(FILE *input, const char *stats_name)
{
    char line[ADMIN_REQUEST_SIZE];
    if (fgets(line, sizeof line, input) == NULL) {
        fprintf(stderr, "no reply from switch\n");
        return 1;
    }
    if (strcmp(line, "OK\n") != 0) {
        fprintf(stderr, "%s", line);
        return 1;
    }
    if (stats_name != NULL) {
        size_t n = strlen(stats_name);
        if (fgets(line, sizeof line, input) == NULL || strncmp(line, stats_name, n) != 0 ||
            line[n] != '\n' || line[n + 1] != '\0') {
            fprintf(stderr, "invalid stats reply\n");
            return 1;
        }
    } else {
        while (fgets(line, sizeof line, input) != NULL) {
            if (fputs(line, stdout) == EOF) return 1;
        }
        if (ferror(input) || fflush(stdout) == EOF) return 1;
    }
    return 0;
}

static int request(const AdminNames *names, const char *line, int stats)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("create admin socket");
        return 1;
    }
    int status = 1;
    if (connect(fd, (const struct sockaddr *)&names->address, sizeof names->address) == -1) {
        perror("connect to switch admin socket");
    } else if (write_full(fd, line, strlen(line)) == -1) {
        perror("send admin command");
    } else {
        FILE *input = fdopen(fd, "r");
        if (input != NULL) {
            status = read_reply(input, stats ? names->stats : NULL);
            if (fclose(input) == EOF) {
                perror("close admin socket");
                status = 1;
            }
            return status;
        }
        perror("open admin reply stream");
    }
    if (close(fd) == -1) perror("close admin socket");
    return status;
}

int main(int argc, char *argv[])
{
    unsigned tcp_port = 9000;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "--port") == 0) {
        if (argc < 4 || parse_number(argv[2], 1, 65535, &tcp_port) == -1) goto usage;
        first = 3;
    }
    if (first >= argc) goto usage;
    int stats = strcmp(argv[first], "stats") == 0;
    int watch = stats && argc == first + 2 && strcmp(argv[first + 1], "--watch") == 0;
    if (stats && argc != first + 1 && !watch) goto usage;

    char line[ADMIN_REQUEST_SIZE];
    size_t length = 0;
    int end = watch ? argc - 1 : argc;
    for (int i = first; i < end; i++) {
        size_t n = strlen(argv[i]);
        if (strpbrk(argv[i], "\r\n") != NULL || n >= sizeof line - length - 1) goto usage;
        memcpy(line + length, argv[i], n);
        length += n;
        line[length++] = i + 1 == end ? '\n' : ' ';
    }
    line[length] = '\0';
    AdminNames names;
    if (admin_names(&names, tcp_port) == -1 || ignore_sigpipe() == -1) {
        perror("set up swctl");
        return 1;
    }
    int status = request(&names, line, stats);
    return status == 0 && stats ? show_stats(names.stats, watch) : status;

usage:
    fprintf(stderr, "usage: ./swctl [--port tcp_port] <command>\n"
                    "commands: show mac | show ports | shutdown PORT | up PORT |\n"
                    "          vlan PORT VLAN | mirror SOURCE SNIFFER | clear mac | stats [--watch]\n");
    return 1;
}
