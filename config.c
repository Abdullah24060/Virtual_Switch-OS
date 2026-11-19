#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "rate_timer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int parse_line(char *line, SwitchOptions *config)
{
    char *comment = strchr(line, '#');
    if (comment != NULL) *comment = '\0';
    char *save;
    char *key = strtok_r(line, " \t\r\n", &save);
    if (key == NULL) return 0;
    char *first = strtok_r(NULL, " \t\r\n", &save);
    char *second = strtok_r(NULL, " \t\r\n", &save);
    if (first == NULL || strtok_r(NULL, " \t\r\n", &save) != NULL) return -1;

    unsigned port, value;
    if (strcmp(key, "rate") == 0 && second == NULL)
        return parse_number(first, 1, MAX_SEND_RATE, &config->send_rate);
    if (strcmp(key, "vlan") == 0 && second != NULL &&
        parse_number(first, 1, config->num_ports, &port) == 0 &&
        parse_number(second, 1, 4094, &value) == 0) {
        config->vlans[port - 1] = (uint16_t)value;
        return 0;
    }
    if (strcmp(key, "trunk") == 0 && second == NULL) {
        if (strcmp(first, "off") != 0)
            return parse_number(first, 1, config->num_ports, &config->trunk);
        config->trunk = 0;
        return 0;
    }
    if (strcmp(key, "mirror") == 0) {
        if (second == NULL && strcmp(first, "off") == 0) {
            config->mirror_source = config->sniffer = 0;
            return 0;
        }
        if (second != NULL && parse_number(first, 1, config->num_ports, &port) == 0 &&
            parse_number(second, 1, config->num_ports, &value) == 0 && port != value) {
            config->mirror_source = port;
            config->sniffer = value;
            return 0;
        }
    }
    return -1;
}

static FILE *open_config(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) return NULL;
    struct stat info;
    FILE *file = NULL;
    if (fstat(fd, &info) == 0) {
        if (!S_ISREG(info.st_mode)) errno = EINVAL;
        else if (info.st_size > 4 * 1024 * 1024) errno = EFBIG;
        else file = fdopen(fd, "r");
    }
    if (file == NULL) {
        int error = errno;
        if (close(fd) == -1) perror("close config");
        errno = error;
    }
    return file;
}

int config_load(const SwitchOptions *base, SwitchOptions *config, unsigned *line_number)
{
    *line_number = 0;
    *config = *base;
    size_t bytes = base->num_ports * sizeof(*base->vlans);
    config->vlans = malloc(bytes);
    if (config->vlans == NULL) return -1;
    memcpy(config->vlans, base->vlans, bytes);
    if (base->config_path == NULL) return 0;

    FILE *file = open_config(base->config_path);
    if (file == NULL) goto fail;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int error = 0;
    while ((length = getline(&line, &capacity, file)) != -1) {
        (*line_number)++;
        if (memchr(line, '\0', (size_t)length) != NULL || parse_line(line, config) == -1) {
            error = EINVAL;
            break;
        }
    }
    if (error == 0) {
        *line_number = 0;
        if (!feof(file)) error = errno ? errno : EIO;
        else if (config->trunk != 0 && config->trunk == config->sniffer) error = EINVAL;
    }
    free(line);
    if (fclose(file) == EOF && error == 0) error = errno;
    if (error == 0) return 0;
    errno = error;
fail:
    free(config->vlans);
    config->vlans = NULL;
    return -1;
}

static int print_dump(FILE *file, const MacRecord *records, size_t count)
{
    if (fprintf(file, "MAC table (%zu entries)\n", count) < 0) return -1;
    for (size_t i = 0; i < count; i++) {
        const MacRecord *row = &records[i];
        if (fprintf(file, "%02x:%02x:%02x:%02x:%02x:%02x vlan=%u port=%u\n",
                    row->mac[0], row->mac[1], row->mac[2], row->mac[3], row->mac[4],
                    row->mac[5], (unsigned)row->vlan, row->port + 1) < 0)
            return -1;
    }
    return 0;
}

int mac_dump_file(MacTable *table, const char *path, size_t *count)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (!S_ISREG(info.st_mode)) {
            errno = EINVAL;
            return -1;
        }
    } else if (errno != ENOENT) return -1;
    MacRecord *records;
    if (mac_snapshot(table, &records, count) == -1) return -1;
    const char suffix[] = ".tmp.XXXXXX";
    size_t length = strlen(path);
    char *temp = malloc(length + sizeof suffix);
    if (temp == NULL) {
        free(records);
        return -1;
    }
    memcpy(temp, path, length);
    memcpy(temp + length, suffix, sizeof suffix);

    int fd = mkstemp(temp);
    int error = fd == -1 ? errno : 0;
    if (fd != -1) {
        FILE *file = fdopen(fd, "w");
        if (file == NULL) {
            error = errno;
            if (close(fd) == -1) perror("close MAC dump");
        } else {
            if (print_dump(file, records, *count) == -1) error = errno ? errno : EIO;
            if (fclose(file) == EOF && error == 0) error = errno;
        }
        if (error == 0 && rename(temp, path) == -1) error = errno;
        if (error != 0 && unlink(temp) == -1) perror("remove temporary MAC dump");
    }
    free(temp);
    free(records);
    if (error == 0) return 0;
    errno = error;
    return -1;
}
