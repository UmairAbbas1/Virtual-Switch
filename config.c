#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "logger.h"

void config_default(struct config *c)
{
    c->rate = 0;
    c->weights[0] = 5;
    c->weights[1] = 3;
    c->weights[2] = 1;
    c->max_wait_ms = 200;
    c->aging = 60;
    c->port_max_macs = 8;
    c->wait_seconds = 5;
    c->mac_table_size = 1024;
    c->trunk = -1;
    for (int i = 0; i < MAX_PORTS; i++)
        c->vlan[i] = 1;
    strcpy(c->log_file, "switch.log");
}

static int bad(const char *path, int line, const char *why)
{
    log_msg("config %s line %d: %s", path, line, why);
    return -1;
}

int config_load(const char *path, struct config *c, int num_ports)
{
    char line[300];
    char key[64];
    int a, b, d;
    int n = 0;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        log_msg("cannot open config file %s", path);
        return -1;
    }
    config_default(c);

    while (fgets(line, sizeof(line), f) != NULL) {
        n++;
        line[strcspn(line, "#\n")] = 0;
        if (sscanf(line, "%63s", key) != 1)
            continue;

        int r = 0;
        if (strcmp(key, "rate") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0)
                r = bad(path, n, "rate must be 0 or more");
            else
                c->rate = a;
        } else if (strcmp(key, "weights") == 0) {
            if (sscanf(line, "%*s %d %d %d", &a, &b, &d) != 3 || a < 1 || b < 1 || d < 1)
                r = bad(path, n, "weights needs three numbers, each 1 or more");
            else {
                c->weights[0] = a;
                c->weights[1] = b;
                c->weights[2] = d;
            }
        } else if (strcmp(key, "max_wait_ms") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0)
                r = bad(path, n, "max_wait_ms must be 0 or more");
            else
                c->max_wait_ms = a;
        } else if (strcmp(key, "aging") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 1)
                r = bad(path, n, "aging must be 1 or more");
            else
                c->aging = a;
        } else if (strcmp(key, "port_max_macs") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 1)
                r = bad(path, n, "port_max_macs must be 1 or more");
            else
                c->port_max_macs = a;
        } else if (strcmp(key, "wait_seconds") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0)
                r = bad(path, n, "wait_seconds must be 0 or more");
            else
                c->wait_seconds = a;
        } else if (strcmp(key, "mac_table_size") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 1)
                r = bad(path, n, "mac_table_size must be 1 or more");
            else
                c->mac_table_size = a;
        } else if (strcmp(key, "trunk") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < -1 || a >= num_ports)
                r = bad(path, n, "trunk must be a port number or -1");
            else
                c->trunk = a;
        } else if (strcmp(key, "vlan") == 0) {
            if (sscanf(line, "%*s %d %d", &a, &b) != 2 || a < 0 || a >= num_ports ||
                b < 1 || b > 4094)
                r = bad(path, n, "use: vlan <port> <1-4094>");
            else
                c->vlan[a] = b;
        } else if (strcmp(key, "log_file") == 0) {
            if (sscanf(line, "%*s %199s", c->log_file) != 1)
                r = bad(path, n, "log_file needs a path");
        } else {
            r = bad(path, n, "unknown setting");
        }
        if (r < 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
