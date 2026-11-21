#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include "frame.h"

void frame_serialize(struct frame_header *h, uint8_t *buf)
{
    uint16_t len = htons(h->length);
    uint16_t vlan = htons(h->vlan);

    memcpy(buf, &len, 2);
    memcpy(buf + 2, h->src, 6);
    memcpy(buf + 8, h->dst, 6);
    memcpy(buf + 14, &vlan, 2);
    buf[16] = h->priority;
}

void frame_deserialize(uint8_t *buf, struct frame_header *h)
{
    uint16_t len;
    uint16_t vlan;

    memcpy(&len, buf, 2);
    memcpy(h->src, buf + 2, 6);
    memcpy(h->dst, buf + 8, 6);
    memcpy(&vlan, buf + 14, 2);
    h->priority = buf[16];

    h->length = ntohs(len);
    h->vlan = ntohs(vlan);
}

void mac_to_str(uint8_t *mac, char *str)
{
    snprintf(str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int str_to_mac(char *str, uint8_t *mac)
{
    unsigned int m[6];

    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (m[i] > 255)
            return -1;
        mac[i] = m[i];
    }
    return 0;
}

int is_broadcast(uint8_t *mac)
{
    for (int i = 0; i < 6; i++)
        if (mac[i] != 0xFF)
            return 0;
    return 1;
}

int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += r;
    }
    return 0;
}

int write_full(int fd, void *buf, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t r = write(fd, (char *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += r;
    }
    return 0;
}

int frame_recv(int fd, struct frame *f)
{
    uint8_t buf[HEADER_SIZE];

    if (read_full(fd, buf, HEADER_SIZE) < 0)
        return -1;
    frame_deserialize(buf, &f->h);
    if (f->h.length > MAX_PAYLOAD)
        return -1;
    if (read_full(fd, f->payload, f->h.length) < 0)
        return -1;
    return 0;
}

/*
 * Like read_full, but also watches wake_fd: when something is written to it,
 * give up and return -1. Used for pipes, which cannot be shutdown() like sockets.
 */
static int read_full_wake(int fd, int wake_fd, void *buf, size_t n)
{
    size_t got = 0;
    struct pollfd pf[2];

    while (got < n) {
        pf[0].fd = fd;
        pf[0].events = POLLIN;
        pf[1].fd = wake_fd;
        pf[1].events = POLLIN;
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pf[1].revents)
            return -1;
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += r;
    }
    return 0;
}

int frame_recv_wake(int fd, int wake_fd, struct frame *f)
{
    uint8_t buf[HEADER_SIZE];

    if (wake_fd < 0)
        return frame_recv(fd, f);
    if (read_full_wake(fd, wake_fd, buf, HEADER_SIZE) < 0)
        return -1;
    frame_deserialize(buf, &f->h);
    if (f->h.length > MAX_PAYLOAD)
        return -1;
    if (read_full_wake(fd, wake_fd, f->payload, f->h.length) < 0)
        return -1;
    return 0;
}

int frame_send(int fd, struct frame *f)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];

    frame_serialize(&f->h, buf);
    memcpy(buf + HEADER_SIZE, f->payload, f->h.length);
    return write_full(fd, buf, HEADER_SIZE + f->h.length);
}

struct frame *frame_new(void)
{
    struct frame *f = malloc(sizeof(struct frame));
    if (f == NULL)
        return NULL;
    f->refs = 1;
    if (pthread_mutex_init(&f->lock, NULL) != 0) {
        free(f);
        return NULL;
    }
    return f;
}

void frame_get(struct frame *f)
{
    pthread_mutex_lock(&f->lock);
    f->refs++;
    pthread_mutex_unlock(&f->lock);
}

void frame_put(struct frame *f)
{
    int last;

    pthread_mutex_lock(&f->lock);
    f->refs--;
    last = (f->refs == 0);
    pthread_mutex_unlock(&f->lock);

    if (last) {
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
}
