#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define NUM_BUCKETS 16
#define MAX_ENTRIES 1024

struct mac_entry {
    uint8_t mac[6];
    int port;
    time_t last_seen;
    struct mac_entry *next;
};

struct mac_table {
    struct mac_entry *buckets[NUM_BUCKETS];
    pthread_mutex_t locks[NUM_BUCKETS];
    int count;
    int max_entries;
    pthread_mutex_t count_lock;
};

int mac_table_init(struct mac_table *t, int max_entries);
int mac_table_lookup(struct mac_table *t, uint8_t *mac);
int mac_table_insert(struct mac_table *t, uint8_t *mac, int port);
void mac_table_remove(struct mac_table *t, uint8_t *mac);
void mac_table_remove_port(struct mac_table *t, int port);
int mac_table_count_port(struct mac_table *t, int port);
void mac_table_age(struct mac_table *t, int max_age);
void mac_table_destroy(struct mac_table *t);

#endif
