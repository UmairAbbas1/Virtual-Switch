#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define HEADER_SIZE 17
#define MAX_PAYLOAD 1500

struct __attribute__((packed)) frame_header {
    uint16_t length;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t vlan;
    uint8_t priority;
};

struct frame {
    struct frame_header h;
    uint8_t payload[MAX_PAYLOAD];
    int refs;
    pthread_mutex_t lock;
};

void frame_serialize(struct frame_header *h, uint8_t *buf);
void frame_deserialize(uint8_t *buf, struct frame_header *h);
void mac_to_str(uint8_t *mac, char *str);
int str_to_mac(char *str, uint8_t *mac);
int is_broadcast(uint8_t *mac);

int read_full(int fd, void *buf, size_t n);
int write_full(int fd, void *buf, size_t n);
int frame_recv(int fd, struct frame *f);
int frame_recv_wake(int fd, int wake_fd, struct frame *f);
int frame_send(int fd, struct frame *f);

struct frame *frame_new(void);
void frame_get(struct frame *f);
void frame_put(struct frame *f);

#endif
