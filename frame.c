#define _POSIX_C_SOURCE 200809L
#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buffer, size_t length)
{
    size_t done = 0;
    unsigned char *bytes = buffer;

    while (done < length) {
        ssize_t n = read(fd, bytes + done, length - done);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int write_full(int fd, const void *buffer, size_t length)
{
    size_t done = 0;
    const unsigned char *bytes = buffer;

    while (done < length) {
        ssize_t n = write(fd, bytes + done, length - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int valid_frame(const Frame *frame)
{
    return frame->length <= MAX_PAYLOAD && frame->vlan >= 1 &&
           frame->vlan <= 4094 && frame->priority <= 2;
}

int frame_read(int fd, Frame *frame)
{
    uint8_t header[FRAME_HEADER_SIZE];
    uint16_t field;
    ssize_t n = read_full(fd, header, sizeof header);

    if (n <= 0) {
        return (int)n;
    }
    if (n != FRAME_HEADER_SIZE) {
        errno = EPROTO;
        return -1;
    }

    /* memcpy avoids unaligned uint16_t accesses; offsets match the manual. */
    memcpy(&field, header, sizeof field);
    frame->length = ntohs(field);
    memcpy(frame->src, header + 2, MAC_SIZE);
    memcpy(frame->dst, header + 8, MAC_SIZE);
    memcpy(&field, header + 14, sizeof field);
    frame->vlan = ntohs(field);
    frame->priority = header[16];
    if (!valid_frame(frame)) {
        errno = EPROTO;
        return -1;
    }

    n = read_full(fd, frame->payload, frame->length);
    if (n < 0) {
        return -1;
    }
    if (n != frame->length) {
        errno = EPROTO;
        return -1;
    }
    return 1;
}

int frame_write(int fd, const Frame *frame)
{
    uint8_t bytes[FRAME_HEADER_SIZE + MAX_PAYLOAD];
    uint16_t field;

    if (!valid_frame(frame)) {
        errno = EINVAL;
        return -1;
    }
    field = htons(frame->length);
    memcpy(bytes, &field, sizeof field);
    memcpy(bytes + 2, frame->src, MAC_SIZE);
    memcpy(bytes + 8, frame->dst, MAC_SIZE);
    field = htons(frame->vlan);
    memcpy(bytes + 14, &field, sizeof field);
    bytes[16] = frame->priority;
    memcpy(bytes + FRAME_HEADER_SIZE, frame->payload, frame->length);
    return write_full(fd, bytes, FRAME_HEADER_SIZE + frame->length);
}

int frame_print(const char *label, const Frame *frame)
{
    if (printf("%s length=%u src=%02x:%02x:%02x:%02x:%02x:%02x "
               "dst=%02x:%02x:%02x:%02x:%02x:%02x vlan=%u priority=%u payload=",
               label, (unsigned)frame->length,
               frame->src[0], frame->src[1], frame->src[2], frame->src[3],
               frame->src[4], frame->src[5], frame->dst[0], frame->dst[1],
               frame->dst[2], frame->dst[3], frame->dst[4], frame->dst[5],
               (unsigned)frame->vlan, (unsigned)frame->priority) < 0) {
        return -1;
    }
    /* Hex also displays zero bytes and non-text payloads without ambiguity. */
    for (size_t i = 0; i < frame->length; i++) {
        if (printf("%02x", frame->payload[i]) < 0) {
            return -1;
        }
    }
    if (putchar('\n') == EOF || fflush(stdout) == EOF) {
        return -1;
    }
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int parse_mac(const char *text, uint8_t mac[MAC_SIZE])
{
    if (strlen(text) != 17) {
        return -1;
    }
    for (size_t i = 0; i < MAC_SIZE; i++) {
        int high = hex_digit(text[i * 3]);
        int low = hex_digit(text[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < MAC_SIZE - 1 && text[i * 3 + 2] != ':')) {
            return -1;
        }
        mac[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

int parse_number(const char *text, unsigned min, unsigned max, unsigned *value)
{
    char *end;
    unsigned long parsed;

    if (*text == '\0') {
        return -1;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

int ignore_sigpipe(void)
{
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) == -1) {
        return -1;
    }
    return sigaction(SIGPIPE, &action, NULL);
}
