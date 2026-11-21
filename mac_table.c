#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mac_table.h"
#include "frame.h"
#include "logger.h"

static int hash_mac(uint8_t *mac)
{
    unsigned int h = 0;
    for (int i = 0; i < 6; i++)
        h = h * 31 + mac[i];
    return h % NUM_BUCKETS;
}

static void change_count(struct mac_table *t, int n)
{
    pthread_mutex_lock(&t->count_lock);
    t->count += n;
    pthread_mutex_unlock(&t->count_lock);
}

static int get_count(struct mac_table *t)
{
    int n;
    pthread_mutex_lock(&t->count_lock);
    n = t->count;
    pthread_mutex_unlock(&t->count_lock);
    return n;
}

static struct mac_entry *find(struct mac_table *t, int b, uint8_t *mac)
{
    struct mac_entry *e = t->buckets[b];
    while (e != NULL) {
        if (memcmp(e->mac, mac, 6) == 0)
            return e;
        e = e->next;
    }
    return NULL;
}

static int update(struct mac_entry *e, int port)
{
    char str[18];

    e->last_seen = time(NULL);
    if (e->port == port)
        return 0;
    mac_to_str(e->mac, str);
    log_msg("MAC moved: %s port %d -> port %d", str, e->port, port);
    e->port = port;
    return 2;
}

static void evict_oldest(struct mac_table *t)
{
    uint8_t old_mac[6];
    time_t old_time = 0;
    int found = 0;
    char str[18];

    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&t->locks[i]);
        for (struct mac_entry *e = t->buckets[i]; e != NULL; e = e->next) {
            if (!found || e->last_seen < old_time) {
                memcpy(old_mac, e->mac, 6);
                old_time = e->last_seen;
                found = 1;
            }
        }
        pthread_mutex_unlock(&t->locks[i]);
    }

    if (found) {
        mac_to_str(old_mac, str);
        log_msg("MAC table full, evicting %s", str);
        mac_table_remove(t, old_mac);
    }
}

int mac_table_init(struct mac_table *t, int max_entries)
{
    for (int i = 0; i < NUM_BUCKETS; i++) {
        t->buckets[i] = NULL;
        if (pthread_mutex_init(&t->locks[i], NULL) != 0)
            return -1;
    }
    t->count = 0;
    t->max_entries = max_entries;
    if (pthread_mutex_init(&t->count_lock, NULL) != 0)
        return -1;
    return 0;
}

int mac_table_lookup(struct mac_table *t, uint8_t *mac)
{
    int b = hash_mac(mac);
    int port = -1;

    pthread_mutex_lock(&t->locks[b]);
    struct mac_entry *e = find(t, b, mac);
    if (e != NULL) {
        port = e->port;
        e->last_seen = time(NULL);
    }
    pthread_mutex_unlock(&t->locks[b]);

    return port;
}

int mac_table_insert(struct mac_table *t, uint8_t *mac, int port)
{
    int b = hash_mac(mac);
    int r;

    pthread_mutex_lock(&t->locks[b]);
    struct mac_entry *e = find(t, b, mac);
    if (e != NULL) {
        r = update(e, port);
        pthread_mutex_unlock(&t->locks[b]);
        return r;
    }
    pthread_mutex_unlock(&t->locks[b]);

    if (get_count(t) >= t->max_entries)
        evict_oldest(t);

    struct mac_entry *n = malloc(sizeof(struct mac_entry));
    if (n == NULL)
        return -1;
    memcpy(n->mac, mac, 6);
    n->port = port;
    n->last_seen = time(NULL);

    pthread_mutex_lock(&t->locks[b]);
    e = find(t, b, mac);
    if (e != NULL) {
        r = update(e, port);
        pthread_mutex_unlock(&t->locks[b]);
        free(n);
        return r;
    }
    n->next = t->buckets[b];
    t->buckets[b] = n;
    pthread_mutex_unlock(&t->locks[b]);

    change_count(t, 1);
    return 1;
}

void mac_table_remove(struct mac_table *t, uint8_t *mac)
{
    int b = hash_mac(mac);
    int removed = 0;

    pthread_mutex_lock(&t->locks[b]);
    struct mac_entry **pp = &t->buckets[b];
    while (*pp != NULL) {
        if (memcmp((*pp)->mac, mac, 6) == 0) {
            struct mac_entry *e = *pp;
            *pp = e->next;
            free(e);
            removed = 1;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&t->locks[b]);

    if (removed)
        change_count(t, -1);
}

void mac_table_remove_port(struct mac_table *t, int port)
{
    int removed = 0;

    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&t->locks[i]);
        struct mac_entry **pp = &t->buckets[i];
        while (*pp != NULL) {
            if ((*pp)->port == port) {
                struct mac_entry *e = *pp;
                *pp = e->next;
                free(e);
                removed++;
            } else {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&t->locks[i]);
    }
    change_count(t, -removed);
}

int mac_table_count_port(struct mac_table *t, int port)
{
    int n = 0;

    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&t->locks[i]);
        for (struct mac_entry *e = t->buckets[i]; e != NULL; e = e->next)
            if (e->port == port)
                n++;
        pthread_mutex_unlock(&t->locks[i]);
    }
    return n;
}

void mac_table_age(struct mac_table *t, int max_age)
{
    time_t now = time(NULL);
    int removed = 0;
    char str[18];

    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&t->locks[i]);
        struct mac_entry **pp = &t->buckets[i];
        while (*pp != NULL) {
            if (now - (*pp)->last_seen > max_age) {
                struct mac_entry *e = *pp;
                mac_to_str(e->mac, str);
                log_msg("MAC aged out: %s", str);
                *pp = e->next;
                free(e);
                removed++;
            } else {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&t->locks[i]);
    }
    change_count(t, -removed);
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
    pthread_mutex_destroy(&t->count_lock);
}

int mac_table_clear(struct mac_table *t)
{
    int removed = 0;

    for (int i = 0; i < NUM_BUCKETS; i++)
        pthread_mutex_lock(&t->locks[i]);

    for (int i = 0; i < NUM_BUCKETS; i++) {
        struct mac_entry *e = t->buckets[i];
        while (e != NULL) {
            struct mac_entry *next = e->next;
            free(e);
            removed++;
            e = next;
        }
        t->buckets[i] = NULL;
    }

    for (int i = NUM_BUCKETS - 1; i >= 0; i--)
        pthread_mutex_unlock(&t->locks[i]);

    change_count(t, -removed);
    return removed;
}

int mac_table_to_text(struct mac_table *t, char *buf, int size)
{
    time_t now = time(NULL);
    char str[18];
    int pos = 0;
    int n = 0;

    pos += snprintf(buf, size, "MAC                PORT  AGE\n");
    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&t->locks[i]);
        for (struct mac_entry *e = t->buckets[i]; e != NULL; e = e->next) {
            if (pos >= size - 64)
                break;
            mac_to_str(e->mac, str);
            pos += snprintf(buf + pos, size - pos, "%s  %4d  %lds\n", str, e->port,
                            (long)(now - e->last_seen));
            n++;
        }
        pthread_mutex_unlock(&t->locks[i]);
    }
    snprintf(buf + pos, size - pos, "%d entries\n", n);
    return n;
}
