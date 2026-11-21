#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "mac_table.h"
#include "frame.h"
#include "logger.h"

/*
 * Locks: one per bucket (striped). count_lock protects count and the free
 * list. Order: bucket locks in ascending order, then count_lock.
 */

static int hash_mac(uint8_t *mac)
{
    unsigned int h = 0;
    for (int i = 0; i < 6; i++)
        h = h * 31 + mac[i];
    return h % NUM_BUCKETS;
}

static int get_count(struct mac_table *t)
{
    int n;
    pthread_mutex_lock(&t->s->count_lock);
    n = t->s->count;
    pthread_mutex_unlock(&t->s->count_lock);
    return n;
}

static int alloc_entry(struct mac_table *t)
{
    struct mac_shared *s = t->s;
    int i;

    pthread_mutex_lock(&s->count_lock);
    i = s->free_head;
    if (i >= 0) {
        s->free_head = s->entries[i].next;
        s->count++;
    }
    pthread_mutex_unlock(&s->count_lock);
    return i;
}

static void free_entry(struct mac_table *t, int i)
{
    struct mac_shared *s = t->s;

    pthread_mutex_lock(&s->count_lock);
    s->entries[i].used = 0;
    s->entries[i].next = s->free_head;
    s->free_head = i;
    s->count--;
    pthread_mutex_unlock(&s->count_lock);
}

static int find(struct mac_table *t, int b, uint8_t *mac)
{
    for (int i = t->s->buckets[b]; i >= 0; i = t->s->entries[i].next)
        if (memcmp(t->s->entries[i].mac, mac, 6) == 0)
            return i;
    return -1;
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

/*
 * Remove every entry in bucket b that matches. Bucket lock must be held.
 * mode 0: mac equal, mode 1: port equal, mode 2: older than max_age, mode 3: all.
 */
static int remove_matching(struct mac_table *t, int b, int mode, uint8_t *mac, int port,
                           int max_age, time_t now)
{
    struct mac_shared *s = t->s;
    int *pp = &s->buckets[b];
    int removed = 0;
    char str[18];

    while (*pp >= 0) {
        int i = *pp;
        struct mac_entry *e = &s->entries[i];
        int match = (mode == 0 && memcmp(e->mac, mac, 6) == 0) ||
                    (mode == 1 && e->port == port) ||
                    (mode == 2 && now - e->last_seen > max_age) || mode == 3;
        if (match) {
            if (mode == 2) {
                mac_to_str(e->mac, str);
                log_msg("MAC aged out: %s", str);
            }
            *pp = e->next;
            free_entry(t, i);
            removed++;
            if (mode == 0)
                break;
        } else {
            pp = &e->next;
        }
    }
    return removed;
}

static void evict_oldest(struct mac_table *t)
{
    uint8_t old_mac[6];
    time_t old_time = 0;
    int found = 0;
    char str[18];

    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&t->s->locks[b]);
        for (int i = t->s->buckets[b]; i >= 0; i = t->s->entries[i].next) {
            struct mac_entry *e = &t->s->entries[i];
            if (!found || e->last_seen < old_time) {
                memcpy(old_mac, e->mac, 6);
                old_time = e->last_seen;
                found = 1;
            }
        }
        pthread_mutex_unlock(&t->s->locks[b]);
    }
    if (found) {
        mac_to_str(old_mac, str);
        log_msg("MAC table full, evicting %s", str);
        mac_table_remove(t, old_mac);
    }
}

static int init_locks(struct mac_shared *s)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
        return -1;
    for (int b = 0; b < NUM_BUCKETS; b++)
        if (pthread_mutex_init(&s->locks[b], &attr) != 0)
            return -1;
    if (pthread_mutex_init(&s->count_lock, &attr) != 0)
        return -1;
    pthread_mutexattr_destroy(&attr);
    return 0;
}

/*
 * Rebuild the bucket chains, free list and count from the "used" flags.
 * Used after a crash: the old process may have died in the middle of a change,
 * so the chains are not trusted, only the entries themselves.
 */
static int rebuild(struct mac_shared *s, int num_ports)
{
    int n = 0;

    s->free_head = -1;
    for (int b = 0; b < NUM_BUCKETS; b++)
        s->buckets[b] = -1;
    for (int i = MAX_ENTRIES - 1; i >= 0; i--) {
        struct mac_entry *e = &s->entries[i];
        if (e->used && e->port >= 0 && e->port < num_ports) {
            int b = hash_mac(e->mac);
            e->next = s->buckets[b];
            s->buckets[b] = i;
            n++;
        } else {
            e->used = 0;
            e->next = s->free_head;
            s->free_head = i;
        }
    }
    s->count = n;
    return n;
}

int mac_table_init(struct mac_table *t, int max_entries, const char *shm_name, int restore,
                   int num_ports)
{
    struct mac_shared *s;
    int fd = -1;

    t->max_entries = max_entries > MAX_ENTRIES ? MAX_ENTRIES : max_entries;
    t->name[0] = 0;
    if (shm_name != NULL) {
        snprintf(t->name, sizeof(t->name), "%s", shm_name);
        fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            perror("shm_open mac table");
            return -1;
        }
        if (ftruncate(fd, sizeof(struct mac_shared)) < 0) {
            perror("ftruncate mac table");
            close(fd);
            return -1;
        }
        s = mmap(NULL, sizeof(struct mac_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    } else {
        s = mmap(NULL, sizeof(struct mac_shared), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    }
    if (s == MAP_FAILED) {
        perror("mmap mac table");
        return -1;
    }
    t->s = s;

    if (restore && s->magic == MAC_MAGIC) {
        /* the old switch is dead, so nobody holds these locks: start them fresh */
        if (init_locks(s) != 0)
            return -1;
        int n = rebuild(s, num_ports);
        log_msg("MAC table restored from shared memory: %d entries", n);
        return 0;
    }

    memset(s, 0, sizeof(struct mac_shared));
    if (init_locks(s) != 0)
        return -1;
    rebuild(s, num_ports);
    s->magic = MAC_MAGIC;
    return 0;
}

int mac_table_lookup(struct mac_table *t, uint8_t *mac)
{
    int b = hash_mac(mac);
    int port = -1;

    pthread_mutex_lock(&t->s->locks[b]);
    int i = find(t, b, mac);
    if (i >= 0) {
        port = t->s->entries[i].port;
        t->s->entries[i].last_seen = time(NULL);
    }
    pthread_mutex_unlock(&t->s->locks[b]);
    return port;
}

int mac_table_insert(struct mac_table *t, uint8_t *mac, int port)
{
    struct mac_shared *s = t->s;
    int b = hash_mac(mac);
    int r;

    pthread_mutex_lock(&s->locks[b]);
    int i = find(t, b, mac);
    if (i >= 0) {
        r = update(&s->entries[i], port);
        pthread_mutex_unlock(&s->locks[b]);
        return r;
    }
    pthread_mutex_unlock(&s->locks[b]);

    if (get_count(t) >= t->max_entries)
        evict_oldest(t);
    int n = alloc_entry(t);
    if (n < 0) {
        evict_oldest(t);
        n = alloc_entry(t);
        if (n < 0)
            return -1;
    }

    pthread_mutex_lock(&s->locks[b]);
    i = find(t, b, mac);
    if (i >= 0) {
        r = update(&s->entries[i], port);
        s->entries[n].used = 0;
        pthread_mutex_unlock(&s->locks[b]);
        pthread_mutex_lock(&s->count_lock);
        s->entries[n].next = s->free_head;
        s->free_head = n;
        s->count--;
        pthread_mutex_unlock(&s->count_lock);
        return r;
    }
    struct mac_entry *e = &s->entries[n];
    memcpy(e->mac, mac, 6);
    e->port = port;
    e->last_seen = time(NULL);
    e->next = s->buckets[b];
    e->used = 1;
    s->buckets[b] = n;
    pthread_mutex_unlock(&s->locks[b]);
    return 1;
}

void mac_table_remove(struct mac_table *t, uint8_t *mac)
{
    int b = hash_mac(mac);

    pthread_mutex_lock(&t->s->locks[b]);
    remove_matching(t, b, 0, mac, 0, 0, 0);
    pthread_mutex_unlock(&t->s->locks[b]);
}

void mac_table_remove_port(struct mac_table *t, int port)
{
    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&t->s->locks[b]);
        remove_matching(t, b, 1, NULL, port, 0, 0);
        pthread_mutex_unlock(&t->s->locks[b]);
    }
}

int mac_table_count_port(struct mac_table *t, int port)
{
    int n = 0;

    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&t->s->locks[b]);
        for (int i = t->s->buckets[b]; i >= 0; i = t->s->entries[i].next)
            if (t->s->entries[i].port == port)
                n++;
        pthread_mutex_unlock(&t->s->locks[b]);
    }
    return n;
}

void mac_table_age(struct mac_table *t, int max_age)
{
    time_t now = time(NULL);

    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&t->s->locks[b]);
        remove_matching(t, b, 2, NULL, 0, max_age, now);
        pthread_mutex_unlock(&t->s->locks[b]);
    }
}

/* takes every bucket lock in ascending order, so the table is cleared in one step */
int mac_table_clear(struct mac_table *t)
{
    int removed = 0;

    for (int b = 0; b < NUM_BUCKETS; b++)
        pthread_mutex_lock(&t->s->locks[b]);
    for (int b = 0; b < NUM_BUCKETS; b++)
        removed += remove_matching(t, b, 3, NULL, 0, 0, 0);
    for (int b = NUM_BUCKETS - 1; b >= 0; b--)
        pthread_mutex_unlock(&t->s->locks[b]);
    return removed;
}

int mac_table_to_text(struct mac_table *t, char *buf, int size)
{
    time_t now = time(NULL);
    char str[18];
    int pos = 0;
    int n = 0;

    pos += snprintf(buf, size, "MAC                PORT  AGE\n");
    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&t->s->locks[b]);
        for (int i = t->s->buckets[b]; i >= 0; i = t->s->entries[i].next) {
            struct mac_entry *e = &t->s->entries[i];
            if (pos >= size - 64)
                break;
            mac_to_str(e->mac, str);
            pos += snprintf(buf + pos, size - pos, "%s  %4d  %lds\n", str, e->port,
                            (long)(now - e->last_seen));
            n++;
        }
        pthread_mutex_unlock(&t->s->locks[b]);
    }
    snprintf(buf + pos, size - pos, "%d entries\n", n);
    return n;
}

/* clean shutdown: the table is not needed any more, so remove the shared memory */
void mac_table_destroy(struct mac_table *t)
{
    for (int b = 0; b < NUM_BUCKETS; b++)
        pthread_mutex_destroy(&t->s->locks[b]);
    pthread_mutex_destroy(&t->s->count_lock);
    munmap(t->s, sizeof(struct mac_shared));
    if (t->name[0])
        shm_unlink(t->name);
}
