#define _POSIX_C_SOURCE 200809L
#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* Part 1: read and display one host at a time. Forwarding follows in Part 2. */
static int receive_host(int client)
{
    Frame frame;
    int result;

    while ((result = frame_read(client, &frame)) == 1) {
        if (frame_print("received", &frame) == -1) {
            perror("print frame");
            return -1;
        }
    }
    if (result == -1) {
        perror("host frame");
    }
    return 0; /* A malformed or disconnected host must not stop the server. */
}

int main(int argc, char *argv[])
{
    unsigned tcp_port, num_ports;
    int listener;
    int reuse = 1;
    int status = 0;
    struct sockaddr_in address = {0};

    if (argc != 3 || parse_number(argv[1], 1, 65535, &tcp_port) == -1 ||
        parse_number(argv[2], 1, 65535, &num_ports) == -1) {
        fprintf(stderr, "usage: ./switch <tcp_port> <num_ports>\n");
        return 1;
    }
    if (ignore_sigpipe() == -1) {
        perror("ignore SIGPIPE");
        return 1;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return 1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)tcp_port);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof address) == -1 ||
        listen(listener, (int)num_ports) == -1) {
        perror("set up listener");
        status = 1;
    } else if (printf("listening tcp_port=%u num_ports=%u (Part 1: one host at a time)\n",
                      tcp_port, num_ports) < 0 || fflush(stdout) == EOF) {
        perror("print startup");
        status = 1;
    } else {
        for (;;) {
            int client = accept(listener, NULL, NULL);
            if (client == -1) {
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;
                }
                perror("accept");
                status = 1;
                break;
            }
            int result = receive_host(client);
            if (close(client) == -1) {
                perror("close host");
                status = 1;
                break;
            }
            if (result == -1) {
                status = 1;
                break;
            }
        }
    }
    if (close(listener) == -1) {
        perror("close listener");
        status = 1;
    }
    return status;
}
