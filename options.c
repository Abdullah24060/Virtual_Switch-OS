#include "options.h"
#include "frame.h"
#include "rate_timer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_switch_usage(void)
{
    fprintf(stderr, "usage: ./switch <tcp_port> <num_ports>"
                    " [mac_capacity macs_per_port age_seconds wait_seconds]"
                    " [--rate frames_per_second] [--vlan port vlan_id]..."
                    " [--trunk port] [--mirror source_port sniffer_port]\n");
}

static int invalid_option(void)
{
    errno = EINVAL;
    return -1;
}

int parse_options(int argc, char *argv[], SwitchOptions *options)
{
    *options = (SwitchOptions){
        .capacity = 1024, .mac_limit = 8, .age_seconds = 60,
        .wait_seconds = 5, .send_rate = DEFAULT_SEND_RATE
    };
    if (argc < 3 || parse_number(argv[1], 1, 65535, &options->tcp_port) == -1 ||
        parse_number(argv[2], 1, 65535, &options->num_ports) == -1)
        return invalid_option();

    int i = 3, have_rate = 0;
    unsigned port, vlan;
    if (i < argc && strncmp(argv[i], "--", 2) != 0) {
        if (argc < 7 || parse_number(argv[3], 1, 1000000, &options->capacity) == -1 ||
            parse_number(argv[4], 1, 65535, &options->mac_limit) == -1 ||
            parse_number(argv[5], 1, 86400, &options->age_seconds) == -1 ||
            parse_number(argv[6], 1, 3600, &options->wait_seconds) == -1)
            return invalid_option();
        i = 7;
    }
    options->vlans = calloc(options->num_ports, sizeof *options->vlans);
    if (options->vlans == NULL) return -1;
    while (i < argc) {
        const char *flag = argv[i++];
        if (strcmp(flag, "--rate") == 0) {
            if (have_rate || argc - i < 1 ||
                parse_number(argv[i++], 1, MAX_SEND_RATE, &options->send_rate) == -1)
                return invalid_option();
            have_rate = 1;
        } else if (strcmp(flag, "--vlan") == 0) {
            if (argc - i < 2 || parse_number(argv[i++], 1, options->num_ports, &port) == -1 ||
                parse_number(argv[i++], 1, 4094, &vlan) == -1 || options->vlans[port - 1] != 0)
                return invalid_option();
            options->vlans[port - 1] = (uint16_t)vlan;
        } else if (strcmp(flag, "--trunk") == 0) {
            if (options->trunk || argc - i < 1 ||
                parse_number(argv[i++], 1, options->num_ports, &options->trunk) == -1)
                return invalid_option();
        } else if (strcmp(flag, "--mirror") == 0) {
            if (options->mirror_source || argc - i < 2 ||
                parse_number(argv[i++], 1, options->num_ports, &options->mirror_source) == -1 ||
                parse_number(argv[i++], 1, options->num_ports, &options->sniffer) == -1 ||
                options->mirror_source == options->sniffer)
                return invalid_option();
        } else {
            return invalid_option();
        }
    }
    if ((options->trunk && options->vlans[options->trunk - 1] != 0) ||
        (options->sniffer && (options->sniffer == options->trunk ||
                             options->vlans[options->sniffer - 1] != 0)))
        return invalid_option();
    return 0;
}
