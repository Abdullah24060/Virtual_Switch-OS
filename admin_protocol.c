#include "admin_protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int admin_names(AdminNames *names, unsigned tcp_port)
{
    uintmax_t uid = (uintmax_t)geteuid();
    *names = (AdminNames){.address = {.sun_family = AF_UNIX}};
    int a = snprintf(names->directory, sizeof names->directory, "/tmp/virtual-switch-%" PRIuMAX, uid);
    int b = snprintf(names->address.sun_path, sizeof names->address.sun_path, "%s/%u.sock",
                     names->directory, tcp_port);
    int c = snprintf(names->stats, sizeof names->stats, "/virtual-switch-%" PRIuMAX "-%u", uid, tcp_port);
    if (a < 0 || b < 0 || c < 0 || (size_t)a >= sizeof names->directory ||
        (size_t)b >= sizeof names->address.sun_path || (size_t)c >= sizeof names->stats) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
