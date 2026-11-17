#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
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

void print_mac(uint8_t *mac)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
