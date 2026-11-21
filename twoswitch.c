#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/*
 * Starts two switches, A and B, joined by two pipes (like two_way.c in Lab 05):
 *   pipe ab: A writes, B reads
 *   pipe ba: B writes, A reads
 * Each switch uses its ends as its last port, the trunk.
 */

pid_t child[2];

void forward_signal(int sig)
{
    for (int i = 0; i < 2; i++)
        if (child[i] > 0)
            kill(child[i], sig);
}

pid_t start_switch(char *name, int rfd, int wfd, int close1, int close2, char *tcp_port,
                   char *num_ports, char *config)
{
    char fds[32];

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        /* only keep our own two ends, or the other switch never sees EOF */
        close(close1);
        close(close2);
        snprintf(fds, sizeof(fds), "%d,%d", rfd, wfd);
        if (config != NULL)
            execl("./switch", "switch", "-n", name, "-l", fds, tcp_port, num_ports, config,
                  (char *)NULL);
        else
            execl("./switch", "switch", "-n", name, "-l", fds, tcp_port, num_ports,
                  (char *)NULL);
        perror("execl ./switch");
        exit(1);
    }
    return pid;
}

int main(int argc, char *argv[])
{
    int ab[2], ba[2];
    struct sigaction sa;
    int status;

    if (argc != 4 && argc != 6) {
        printf("usage: %s <tcp_port_A> <tcp_port_B> <num_ports> [config_A config_B]\n", argv[0]);
        return 1;
    }
    if (pipe(ab) < 0 || pipe(ba) < 0) {
        perror("pipe");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    child[0] = start_switch("A", ba[0], ab[1], ab[0], ba[1], argv[1], argv[3],
                            argc == 6 ? argv[4] : NULL);
    child[1] = start_switch("B", ab[0], ba[1], ba[0], ab[1], argv[2], argv[3],
                            argc == 6 ? argv[5] : NULL);
    close(ab[0]);
    close(ab[1]);
    close(ba[0]);
    close(ba[1]);
    printf("switch A (pid %d) on TCP %s, switch B (pid %d) on TCP %s, trunk port %d\n",
           child[0], argv[1], child[1], argv[2], atoi(argv[3]) - 1);
    printf("use: swctl -n A ...  and  swctl -n B ...\n");
    fflush(stdout);

    for (int left = 2; left > 0;) {
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            return 1;
        }
        for (int i = 0; i < 2; i++)
            if (child[i] == pid) {
                printf("switch %c exited (status %d)\n", 'A' + i,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                child[i] = 0;
                left--;
            }
    }
    return 0;
}
