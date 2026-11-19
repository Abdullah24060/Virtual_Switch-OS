#ifndef ADMIN_H
#define ADMIN_H

#include "admin_protocol.h"
#include "admission.h"
#include "options.h"

typedef struct {
    AdminNames names;
    Admission *admission;
    MacTable *mac;
    Logger *logger;
    Port *mirror_source, *sniffer;
    int listener, wake[2], client, stopping;
    pthread_mutex_t client_lock;
    pthread_t thread;
} Admin;

int admin_start(Admin *admin, const AdminNames *names, Admission *admission, MacTable *mac,
                Logger *logger);
void admin_apply_config(Admin *admin, const SwitchOptions *config);
void admin_stop(Admin *admin);

#endif
