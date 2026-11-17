#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <pthread.h>

#define NUM_BUCKETS 16

struct mac_entry {
    uint8_t mac[6];
    int port;
    struct mac_entry *next;
};

struct mac_table {
    struct mac_entry *buckets[NUM_BUCKETS];
    pthread_mutex_t locks[NUM_BUCKETS];
};

int mac_table_init(struct mac_table *t);
int mac_table_lookup(struct mac_table *t, uint8_t *mac);
int mac_table_insert(struct mac_table *t, uint8_t *mac, int port);
void mac_table_destroy(struct mac_table *t);

#endif
