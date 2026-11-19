#ifndef ADMIN_PROTOCOL_H
#define ADMIN_PROTOCOL_H

#include <sys/un.h>

#define ADMIN_NAME_SIZE 64
#define ADMIN_REQUEST_SIZE 256

/* TCP port is used only to select the instance's local socket and stats names. */
typedef struct {
    char directory[ADMIN_NAME_SIZE];
    struct sockaddr_un address;
    char stats[ADMIN_NAME_SIZE];
} AdminNames;

int admin_names(AdminNames *names, unsigned tcp_port);

#endif
