#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define NUM_BUCKETS 16
#define MAX_ENTRIES 1024
#define MAC_MAGIC 0x56535754

/* entries are linked by array index (not pointers) so the table can live in shared memory */
struct mac_entry {
    uint8_t mac[6];
    int port;
    time_t last_seen;
    int next;
    int used;
};

struct mac_shared {
    int magic;
    int count;
    int free_head;
    int buckets[NUM_BUCKETS];
    pthread_mutex_t locks[NUM_BUCKETS];
    pthread_mutex_t count_lock;
    struct mac_entry entries[MAX_ENTRIES];
};

struct mac_table {
    struct mac_shared *s;
    int max_entries;
    char name[64];
};

int mac_table_init(struct mac_table *t, int max_entries, const char *shm_name, int restore,
                   int num_ports);
int mac_table_lookup(struct mac_table *t, uint8_t *mac);
int mac_table_insert(struct mac_table *t, uint8_t *mac, int port);
void mac_table_remove(struct mac_table *t, uint8_t *mac);
void mac_table_remove_port(struct mac_table *t, int port);
int mac_table_count_port(struct mac_table *t, int port);
void mac_table_age(struct mac_table *t, int max_age);
int mac_table_clear(struct mac_table *t);
int mac_table_to_text(struct mac_table *t, char *buf, int size);
void mac_table_destroy(struct mac_table *t);

#endif
