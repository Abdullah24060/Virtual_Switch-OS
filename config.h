#ifndef CONFIG_H
#define CONFIG_H

#include "options.h"
#include "mac_table.h"

/* Build a separate copy. On failure the current settings can stay in use. */
int config_load(const SwitchOptions *base, SwitchOptions *config, unsigned *line_number);
int mac_dump_file(MacTable *table, const char *path, size_t *count);

#endif
