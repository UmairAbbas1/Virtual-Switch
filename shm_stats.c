#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "shm_stats.h"

struct shm_stats *stats_create(const char *name, int num_ports)
{
    pthread_mutexattr_t attr;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof(struct shm_stats)) < 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    struct shm_stats *s = mmap(NULL, sizeof(struct shm_stats), PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    memset(s, 0, sizeof(struct shm_stats));
    s->num_ports = num_ports;

    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&s->lock, &attr) != 0) {
        printf("shared mutex init failed\n");
        munmap(s, sizeof(struct shm_stats));
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return s;
}

struct shm_stats *stats_open(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0)
        return NULL;
    struct shm_stats *s = mmap(NULL, sizeof(struct shm_stats), PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED)
        return NULL;
    return s;
}

void stats_add(struct shm_stats *s, int port, int which)
{
    pthread_mutex_lock(&s->lock);
    if (which == STAT_IN)
        s->ports[port].in++;
    else if (which == STAT_OUT)
        s->ports[port].out++;
    else if (which == STAT_DROP)
        s->ports[port].dropped++;
    else if (which == STAT_MIRROR)
        s->ports[port].mirrored++;
    pthread_mutex_unlock(&s->lock);
}

int stats_read(struct shm_stats *s, struct port_stats *out)
{
    int n;

    pthread_mutex_lock(&s->lock);
    n = s->num_ports;
    memcpy(out, s->ports, sizeof(struct port_stats) * n);
    pthread_mutex_unlock(&s->lock);
    return n;
}

void stats_close(struct shm_stats *s)
{
    munmap(s, sizeof(struct shm_stats));
}

void stats_destroy(struct shm_stats *s, const char *name)
{
    pthread_mutex_destroy(&s->lock);
    munmap(s, sizeof(struct shm_stats));
    shm_unlink(name);
}
