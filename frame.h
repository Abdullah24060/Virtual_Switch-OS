#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAC_SIZE 6
#define FRAME_HEADER_SIZE 17
#define MAX_PAYLOAD 1500

/* This is an in-memory structure, never a wire header. */
typedef struct {
    uint16_t length;
    uint8_t src[MAC_SIZE];
    uint8_t dst[MAC_SIZE];
    uint16_t vlan;
    uint8_t priority;
    uint8_t payload[MAX_PAYLOAD];
} Frame;

/* read_full returns the bytes read (possibly short on EOF), or -1. */
ssize_t read_full(int fd, void *buffer, size_t length);
int write_full(int fd, const void *buffer, size_t length);

/* frame_read: 1 = frame, 0 = clean EOF, -1 = error or truncated frame. */
int frame_read(int fd, Frame *frame);
int frame_write(int fd, const Frame *frame);
int frame_print(const char *label, const Frame *frame);
int parse_mac(const char *text, uint8_t mac[MAC_SIZE]);
int parse_number(const char *text, unsigned min, unsigned max, unsigned *value);
int ignore_sigpipe(void);

#endif
