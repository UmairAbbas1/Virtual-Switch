#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "frame.h"

int sock;

void *receiver(void *arg)
{
    struct frame f;
    char src[18];

    (void)arg;
    while (frame_recv(sock, &f) == 0) {
        mac_to_str(f.h.src, src);
        printf("from %s vlan %d prio %d: %.*s\n", src, f.h.vlan, f.h.priority,
               f.h.length, (char *)f.payload);
        fflush(stdout);
    }
    printf("disconnected from switch\n");
    fflush(stdout);
    exit(0);
    return NULL;
}

int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    struct frame f;
    uint8_t my_mac[6];
    char line[1600];
    char dst[32];
    pthread_t th;

    if (argc != 4) {
        printf("usage: %s <switch_ip> <tcp_port> <mac>\n", argv[0]);
        return 1;
    }
    if (str_to_mac(argv[3], my_mac) < 0) {
        printf("bad mac\n");
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        printf("bad ip\n");
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    if (pthread_create(&th, NULL, receiver, NULL) != 0) {
        printf("pthread_create failed\n");
        return 1;
    }

    printf("connected, type: <dst_mac> <message>  or  all <message>\n");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        int n = 0;
        line[strcspn(line, "\n")] = 0;
        if (sscanf(line, "%31s %n", dst, &n) != 1)
            continue;

        if (strcmp(dst, "all") == 0) {
            memset(f.h.dst, 0xFF, 6);
        } else if (str_to_mac(dst, f.h.dst) < 0) {
            printf("bad mac\n");
            continue;
        }
        memcpy(f.h.src, my_mac, 6);
        f.h.vlan = 1;
        f.h.priority = 1;
        f.h.length = strlen(line + n);
        if (f.h.length > MAX_PAYLOAD)
            f.h.length = MAX_PAYLOAD;
        memcpy(f.payload, line + n, f.h.length);

        if (frame_send(sock, &f) < 0) {
            printf("send failed\n");
            break;
        }
    }

    pthread_join(th, NULL);
    return 0;
}
