#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mac_table.h"
#include "frame.h"

static int hash_mac(uint8_t *mac)
{
    unsigned int h = 0;
    for (int i = 0; i < 6; i++)
        h = h * 31 + mac[i];
    return h % NUM_BUCKETS;
}

int mac_table_init(struct mac_table *t)
{
    for (int i = 0; i < NUM_BUCKETS; i++) {
        t->buckets[i] = NULL;
        if (pthread_mutex_init(&t->locks[i], NULL) != 0)
            return -1;
    }
    return 0;
}

int mac_table_lookup(struct mac_table *t, uint8_t *mac)
{
    int b = hash_mac(mac);
    int port = -1;

    pthread_mutex_lock(&t->locks[b]);
    struct mac_entry *e = t->buckets[b];
    while (e != NULL) {
        if (memcmp(e->mac, mac, 6) == 0) {
            port = e->port;
            break;
        }
        e = e->next;
    }
    pthread_mutex_unlock(&t->locks[b]);

    return port;
}

int mac_table_insert(struct mac_table *t, uint8_t *mac, int port)
{
    int b = hash_mac(mac);

    pthread_mutex_lock(&t->locks[b]);
    struct mac_entry *e = t->buckets[b];
    while (e != NULL) {
        if (memcmp(e->mac, mac, 6) == 0) {
            if (e->port != port) {
                printf("MAC moved: ");
                print_mac(mac);
                printf(" port %d -> port %d\n", e->port, port);
                e->port = port;
            }
            pthread_mutex_unlock(&t->locks[b]);
            return 0;
        }
        e = e->next;
    }

    e = malloc(sizeof(struct mac_entry));
    if (e == NULL) {
        pthread_mutex_unlock(&t->locks[b]);
        return -1;
    }
    memcpy(e->mac, mac, 6);
    e->port = port;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    pthread_mutex_unlock(&t->locks[b]);

    return 0;
}

void mac_table_destroy(struct mac_table *t)
{
    for (int i = 0; i < NUM_BUCKETS; i++) {
        struct mac_entry *e = t->buckets[i];
        while (e != NULL) {
            struct mac_entry *next = e->next;
            free(e);
            e = next;
        }
        t->buckets[i] = NULL;
        pthread_mutex_destroy(&t->locks[i]);
    }
}
