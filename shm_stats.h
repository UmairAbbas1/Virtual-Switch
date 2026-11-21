#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <pthread.h>

#define MAX_PORTS 64

#define STAT_IN 0
#define STAT_OUT 1
#define STAT_DROP 2
#define STAT_MIRROR 3

struct port_stats {
    long in;
    long out;
    long dropped;
    long mirrored;
};

struct shm_stats {
    pthread_mutex_t lock;
    int num_ports;
    struct port_stats ports[MAX_PORTS];
};

struct shm_stats *stats_create(const char *name, int num_ports);
struct shm_stats *stats_open(const char *name);
void stats_add(struct shm_stats *s, int port, int which);
int stats_read(struct shm_stats *s, struct port_stats *out);
void stats_close(struct shm_stats *s);
void stats_destroy(struct shm_stats *s, const char *name);

#endif
