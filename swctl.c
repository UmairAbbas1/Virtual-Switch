#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "frame.h"
#include "shm_stats.h"

char sock_path[108] = "/tmp/vswitch.sock";
char stats_name[64] = "/vswitch_stats";

void print_stats(struct shm_stats *s)
{
    struct port_stats ps[MAX_PORTS];
    int n = stats_read(s, ps);

    printf("PORT  IN        OUT       DROPPED   MIRRORED\n");
    for (int i = 0; i < n; i++)
        printf("%-4d  %-8ld  %-8ld  %-8ld  %-8ld\n", i,
               ps[i].in, ps[i].out, ps[i].dropped, ps[i].mirrored);
}

int stats_command(int watch)
{
    struct shm_stats *s = stats_open(stats_name);
    if (s == NULL) {
        printf("cannot open shared memory, is the switch running?\n");
        return 1;
    }
    if (!watch) {
        print_stats(s);
    } else {
        while (1) {
            printf("\033[H\033[J");
            print_stats(s);
            printf("\nrefreshing every 1 second, Ctrl+C to quit\n");
            fflush(stdout);
            sleep(1);
        }
    }
    stats_close(s);
    return 0;
}

int send_command(char *cmd)
{
    struct sockaddr_un addr;
    char buf[4096];
    ssize_t n;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("cannot connect to switch at %s\n", sock_path);
        close(fd);
        return 1;
    }
    if (write_full(fd, cmd, strlen(cmd)) < 0 || write_full(fd, "\n", 1) < 0) {
        printf("send failed\n");
        close(fd);
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, n, stdout);
    if (n < 0)
        perror("read");
    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    char cmd[256] = "";

    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        snprintf(sock_path, sizeof(sock_path), "/tmp/vswitch-%s.sock", argv[2]);
        snprintf(stats_name, sizeof(stats_name), "/vswitch_stats-%s", argv[2]);
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        printf("usage: swctl [-n switch_name] <command>\n");
        printf("commands:\n"
               "  show mac | show ports | clear mac | stats | stats --watch\n"
               "  shutdown <port> | up <port> | vlan <port> <vlan>\n"
               "  mirror <port> <sniffer_port> | mirror <port> off\n"
               "  trunk <port> | trunk off | rate <port> <frames_per_sec>\n");
        return 1;
    }

    if (strcmp(argv[1], "stats") == 0) {
        if (argc == 3 && strcmp(argv[2], "--watch") == 0)
            return stats_command(1);
        return stats_command(0);
    }

    for (int i = 1; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 > sizeof(cmd)) {
            printf("command too long\n");
            return 1;
        }
        if (i > 1)
            strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }
    return send_command(cmd);
}
