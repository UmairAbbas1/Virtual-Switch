#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "logger.h"

#define LOG_QUEUE 512
#define LOG_LINE 256

static char queue[LOG_QUEUE][LOG_LINE];
static int head;
static int tail;
static int count;
static long dropped;
static int running;
static int stopping;
static FILE *file;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

static void *logger_thread(void *arg)
{
    char line[LOG_LINE];

    (void)arg;
    pthread_mutex_lock(&lock);
    while (1) {
        while (count == 0 && !stopping)
            pthread_cond_wait(&not_empty, &lock);
        if (count == 0)
            break;
        memcpy(line, queue[head], LOG_LINE);
        head = (head + 1) % LOG_QUEUE;
        count--;
        pthread_mutex_unlock(&lock);

        fputs(line, file);
        fputs(line, stdout);
        fflush(file);
        fflush(stdout);

        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int log_start(const char *path)
{
    file = fopen(path, "a");
    if (file == NULL) {
        perror("log file");
        return -1;
    }
    pthread_mutex_lock(&lock);
    running = 1;
    stopping = 0;
    pthread_mutex_unlock(&lock);
    if (pthread_create(&thread, NULL, logger_thread, NULL) != 0) {
        printf("cannot start logger thread\n");
        fclose(file);
        return -1;
    }
    return 0;
}

void log_msg(const char *fmt, ...)
{
    char line[LOG_LINE];
    struct timespec ts;
    struct tm tm;
    va_list ap;
    int n;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    n = strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S", &tm);
    n += snprintf(line + n, sizeof(line) - n, ".%03ld ", ts.tv_nsec / 1000000);

    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");

    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        fputs(line, stdout);
        return;
    }
    if (count == LOG_QUEUE) {
        dropped++;
        pthread_mutex_unlock(&lock);
        return;
    }
    memcpy(queue[tail], line, LOG_LINE);
    tail = (tail + 1) % LOG_QUEUE;
    count++;
    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&lock);
}

void log_stop(void)
{
    long lost;

    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        return;
    }
    stopping = 1;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&lock);

    pthread_join(thread, NULL);

    pthread_mutex_lock(&lock);
    running = 0;
    lost = dropped;
    pthread_mutex_unlock(&lock);

    if (lost > 0)
        fprintf(file, "(%ld log messages dropped because the log queue was full)\n", lost);
    if (fclose(file) != 0)
        perror("fclose log");
    file = NULL;
}
