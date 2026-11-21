#ifndef CONFIG_H
#define CONFIG_H

#include "shm_stats.h"

struct config {
    int rate;
    int weights[3];
    int max_wait_ms;
    int aging;
    int port_max_macs;
    int wait_seconds;
    int mac_table_size;
    int trunk;
    int vlan[MAX_PORTS];
    char log_file[200];
};

void config_default(struct config *c);
int config_load(const char *path, struct config *c, int num_ports);

#endif
