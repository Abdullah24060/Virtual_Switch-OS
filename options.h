#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdint.h>

typedef struct {
    unsigned tcp_port, num_ports;
    unsigned capacity, mac_limit, age_seconds, wait_seconds, send_rate;
    unsigned trunk, mirror_source, sniffer; /* One-based port numbers; zero if unused. */
    uint16_t *vlans; /* Indexed by port number minus one; zero means default. */
    const char *config_path, *log_path, *dump_path;
} SwitchOptions;

/* The caller owns options->vlans, even if parsing an option fails. */
int parse_options(int argc, char *argv[], SwitchOptions *options);
void print_switch_usage(void);

#endif
