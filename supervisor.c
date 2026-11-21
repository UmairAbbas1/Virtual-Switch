#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

/*
 * Runs ./switch as a child and restarts it if it crashes.
 * A restart passes -r, so the switch takes its MAC table back from shared memory.
 * A clean exit (status 0, e.g. after Ctrl+C) stops the supervisor too.
 */

#define MAX_RESTARTS 10

volatile sig_atomic_t got_stop = 0;
volatile pid_t child = 0;

void on_stop(int sig)
{
    got_stop = 1;
    if (child > 0)
        kill(child, sig);
}

int main(int argc, char *argv[])
{
    struct sigaction sa;
    char *args[64];
    int restarts = 0;
    int status;

    if (argc < 3 || argc > 60) {
        printf("usage: %s <switch arguments>\n", argv[0]);
        printf("example: %s 9000 4 switch.conf\n", argv[0]);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    while (1) {
        int n = 0;
        args[n++] = "switch";
        if (restarts > 0)
            args[n++] = "-r";
        for (int i = 1; i < argc; i++)
            args[n++] = argv[i];
        args[n] = NULL;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            execv("./switch", args);
            perror("execv ./switch");
            _exit(127);
        }
        child = pid;
        printf("supervisor: switch started, pid %d%s\n", pid,
               restarts > 0 ? " (restoring MAC table)" : "");
        fflush(stdout);

        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                perror("waitpid");
                return 1;
            }
        }
        child = 0;

        if (WIFEXITED(status) && (WEXITSTATUS(status) == 0 || got_stop)) {
            printf("supervisor: switch exited normally, stopping\n");
            return 0;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            return 1;
        if (WIFSIGNALED(status))
            printf("supervisor: switch crashed (signal %d: %s)\n", WTERMSIG(status),
                   strsignal(WTERMSIG(status)));
        else
            printf("supervisor: switch exited with status %d\n", WEXITSTATUS(status));
        if (got_stop)
            return 0;
        if (++restarts > MAX_RESTARTS) {
            printf("supervisor: too many crashes, giving up\n");
            return 1;
        }
        fflush(stdout);
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (got_stop)
            return 0;
    }
}
