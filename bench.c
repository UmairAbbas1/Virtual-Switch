#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/*
 * Benchmark: N threads use one MAC table at the same time.
 * 90% of operations are lookups and 10% are inserts/updates, like a switch
 * where most frames go to MACs it already knows.
 * The table is the same each time (16 buckets, chained), only the locking changes:
 *   global  - one mutex for the whole table
 *   striped - one mutex per bucket (what the switch uses)
 *   rwlock  - one read-write lock: lookups share it, inserts take it alone
 */

#define BUCKETS 16
#define KEYS 512
#define OPS 400000
#define RUNS 3

enum { GLOBAL, STRIPED, RWLOCK };
const char *mode_name[] = {"global", "striped", "rwlock"};

struct entry {
    uint8_t mac[6];
    int port;
    struct entry *next;
};

struct entry *buckets[BUCKETS];
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stripe[BUCKETS];
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
struct entry pool[KEYS];
uint8_t keys[KEYS][6];
int mode;

int hash_mac(uint8_t *mac)
{
    unsigned int h = 0;
    for (int i = 0; i < 6; i++)
        h = h * 31 + mac[i];
    return h % BUCKETS;
}

void lock(int b, int write)
{
    if (mode == GLOBAL)
        pthread_mutex_lock(&global_lock);
    else if (mode == STRIPED)
        pthread_mutex_lock(&stripe[b]);
    else if (write)
        pthread_rwlock_wrlock(&rw);
    else
        pthread_rwlock_rdlock(&rw);
}

void unlock(int b)
{
    if (mode == GLOBAL)
        pthread_mutex_unlock(&global_lock);
    else if (mode == STRIPED)
        pthread_mutex_unlock(&stripe[b]);
    else
        pthread_rwlock_unlock(&rw);
}

struct entry *find(int b, uint8_t *mac)
{
    for (struct entry *e = buckets[b]; e != NULL; e = e->next)
        if (memcmp(e->mac, mac, 6) == 0)
            return e;
    return NULL;
}

int lookup(uint8_t *mac)
{
    int b = hash_mac(mac);
    int port = -1;

    lock(b, 0);
    struct entry *e = find(b, mac);
    if (e != NULL)
        port = e->port;
    unlock(b);
    return port;
}

void update(uint8_t *mac, int port)
{
    int b = hash_mac(mac);

    lock(b, 1);
    struct entry *e = find(b, mac);
    if (e != NULL)
        e->port = port;
    unlock(b);
}

void *worker(void *arg)
{
    unsigned int seed = (unsigned int)(long)arg * 7919 + 1;
    long found = 0;

    for (int i = 0; i < OPS; i++) {
        int k = rand_r(&seed) % KEYS;
        if (rand_r(&seed) % 10 == 0)
            update(keys[k], i & 63);
        else if (lookup(keys[k]) >= 0)
            found++;
    }
    return (void *)found;
}

double run(int threads)
{
    pthread_t th[64];
    struct timespec a, b;

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < threads; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            printf("pthread_create failed\n");
            exit(1);
        }
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);

    double secs = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    return (double)threads * OPS / secs / 1e6;
}

int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    int counts[] = {1, 2, 4, 8, 12};
    int nc = sizeof(counts) / sizeof(counts[0]);
    double result[3][5];

    for (int b = 0; b < BUCKETS; b++)
        if (pthread_mutex_init(&stripe[b], NULL) != 0) {
            printf("mutex init failed\n");
            return 1;
        }
    for (int k = 0; k < KEYS; k++) {
        uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, (uint8_t)(k >> 8), (uint8_t)k};
        memcpy(keys[k], mac, 6);
        memcpy(pool[k].mac, mac, 6);
        pool[k].port = k % 8;
        int b = hash_mac(mac);
        pool[k].next = buckets[b];
        buckets[b] = &pool[k];
    }

    printf("Million operations per second (median of %d runs, %d ops per thread)\n", RUNS, OPS);
    printf("threads   global   striped   rwlock\n");
    for (int c = 0; c < nc; c++) {
        for (mode = 0; mode < 3; mode++) {
            double r[RUNS];
            for (int i = 0; i < RUNS; i++)
                r[i] = run(counts[c]);
            qsort(r, RUNS, sizeof(double), cmp);
            result[mode][c] = r[RUNS / 2];
        }
        printf("%7d   %6.2f   %7.2f   %6.2f\n", counts[c], result[0][c], result[1][c],
               result[2][c]);
        fflush(stdout);
    }

    FILE *f = fopen("bench.csv", "w");
    if (f == NULL) {
        perror("bench.csv");
        return 1;
    }
    fprintf(f, "threads,global,striped,rwlock\n");
    for (int c = 0; c < nc; c++)
        fprintf(f, "%d,%.2f,%.2f,%.2f\n", counts[c], result[0][c], result[1][c], result[2][c]);
    if (fclose(f) != 0)
        perror("bench.csv");
    printf("results written to bench.csv\n");

    for (int b = 0; b < BUCKETS; b++)
        pthread_mutex_destroy(&stripe[b]);
    return 0;
}
