#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define HEADER_SIZE 17
#define MAX_PAYLOAD 1500

struct __attribute__((packed)) frame_header {
    uint16_t length;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t vlan;
    uint8_t priority;
};

void frame_serialize(struct frame_header *h, uint8_t *buf);
void frame_deserialize(uint8_t *buf, struct frame_header *h);
void print_mac(uint8_t *mac);

#endif
