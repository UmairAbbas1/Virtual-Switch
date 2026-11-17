#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "frame.h"
#include "mac_table.h"

struct mac_table table;

void *worker(void *arg)
{
    int id = *(int *)arg;
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x00};

    for (int i = 0; i < 100; i++) {
        mac[5] = i;
        mac[4] = id;
        if (mac_table_insert(&table, mac, id) != 0)
            printf("insert failed\n");
        mac_table_lookup(&table, mac);
    }
    return NULL;
}

int main(void)
{
    struct frame_header h, h2;
    uint8_t buf[HEADER_SIZE];
    uint8_t a[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
    uint8_t b[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    printf("header size = %zu\n", sizeof(struct frame_header));

    h.length = 1500;
    memcpy(h.src, a, 6);
    memcpy(h.dst, b, 6);
    h.vlan = 10;
    h.priority = 1;

    frame_serialize(&h, buf);
    printf("bytes: ");
    for (int i = 0; i < HEADER_SIZE; i++)
        printf("%02X ", buf[i]);
    printf("\n");

    frame_deserialize(buf, &h2);
    printf("length=%d vlan=%d priority=%d src=", h2.length, h2.vlan, h2.priority);
    print_mac(h2.src);
    printf(" dst=");
    print_mac(h2.dst);
    printf("\n");

    if (mac_table_init(&table) != 0) {
        printf("init failed\n");
        return 1;
    }

    mac_table_insert(&table, a, 1);
    printf("lookup a -> port %d\n", mac_table_lookup(&table, a));
    mac_table_insert(&table, a, 3);
    printf("lookup a -> port %d\n", mac_table_lookup(&table, a));
    printf("lookup unknown -> port %d\n", mac_table_lookup(&table, b));

    pthread_t th[4];
    int ids[4];
    for (int i = 0; i < 4; i++) {
        ids[i] = i + 1;
        if (pthread_create(&th[i], NULL, worker, &ids[i]) != 0) {
            printf("pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    printf("threads done\n");
    mac_table_destroy(&table);
    return 0;
}
