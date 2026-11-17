#define _POSIX_C_SOURCE 200809L
#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A line is: destination-MAC VLAN priority [text payload]. */
static int send_line(int sock, char *line, const uint8_t src[MAC_SIZE])
{
    Frame frame = {0};
    char *save;
    char *dst = strtok_r(line, " \t\n", &save);
    char *vlan_text = strtok_r(NULL, " \t\n", &save);
    char *priority_text = strtok_r(NULL, " \t\n", &save);
    unsigned vlan, priority;

    if (dst == NULL) {
        return 0;
    }
    if (vlan_text == NULL || priority_text == NULL ||
        parse_mac(dst, frame.dst) == -1 ||
        parse_number(vlan_text, 1, 4094, &vlan) == -1 ||
        parse_number(priority_text, 0, 2, &priority) == -1) {
        fprintf(stderr, "input: <dst_mac> <vlan 1-4094> <priority 0-2> [payload]\n");
        return 0;
    }
    while (*save == ' ' || *save == '\t') {
        save++;
    }
    size_t length = strcspn(save, "\n");
    if (length > MAX_PAYLOAD) {
        fprintf(stderr, "payload must be at most %d bytes\n", MAX_PAYLOAD);
        return 0;
    }
    frame.length = (uint16_t)length;
    frame.vlan = (uint16_t)vlan;
    frame.priority = (uint8_t)priority;
    memcpy(frame.src, src, MAC_SIZE);
    memcpy(frame.payload, save, length);
    if (frame_write(sock, &frame) == -1) {
        perror("send frame"); /* Includes EPIPE after the peer disconnects. */
        return -1;
    }
    return 0;
}

static int run_host(int sock, const uint8_t src[MAC_SIZE])
{
    char line[MAX_PAYLOAD + 128];
    struct pollfd fds[2] = {
        { .fd = sock, .events = POLLIN },
        { .fd = STDIN_FILENO, .events = POLLIN }
    };

    /* poll must see every line, including lines supplied through a pipe. */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fprintf(stderr, "could not configure stdin buffering\n");
        return 1;
    }
    for (;;) {
        if (poll(fds, 2, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            Frame frame;
            int result = frame_read(sock, &frame);
            if (result == 0) return 0;
            if (result == -1) {
                perror("receive frame");
                return 1;
            }
            if (frame_print("received", &frame) == -1) {
                perror("print frame");
                return 1;
            }
        }
        if ((fds[0].revents | fds[1].revents) & POLLNVAL) {
            fprintf(stderr, "invalid poll descriptor\n");
            return 1;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (fgets(line, sizeof line, stdin) == NULL) {
                if (ferror(stdin)) {
                    perror("stdin");
                    return 1;
                }
                return 0;
            }
            if (strchr(line, '\n') == NULL && !feof(stdin)) {
                int c;
                do {
                    c = getchar();
                } while (c != '\n' && c != EOF);
                if (ferror(stdin)) {
                    perror("stdin");
                    return 1;
                }
                fprintf(stderr, "input line too long\n");
                continue;
            }
            if (send_line(sock, line, src) == -1) return 1;
        }
    }
}

int main(int argc, char *argv[])
{
    unsigned tcp_port;
    uint8_t src[MAC_SIZE];
    struct sockaddr_in address = {0};
    int sock;
    int status;

    if (argc != 4 || parse_number(argv[2], 1, 65535, &tcp_port) == -1 ||
        parse_mac(argv[3], src) == -1) {
        fprintf(stderr, "usage: ./host <switch_ip> <tcp_port> <mac>\n");
        return 1;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)tcp_port);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        fprintf(stderr, "switch_ip must be a numeric IPv4 address\n");
        return 1;
    }
    if (ignore_sigpipe() == -1) {
        perror("ignore SIGPIPE");
        return 1;
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&address, sizeof address) == -1) {
        perror("connect");
        status = 1;
    } else {
        status = run_host(sock, src);
    }
    if (close(sock) == -1) {
        perror("close socket");
        status = 1;
    }
    return status;
}
