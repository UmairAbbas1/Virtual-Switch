#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "frame.h"
#include "mac_table.h"

#define QUEUE_SIZE 64
#define PORT_MAX_MACS 8
#define WAIT_SECONDS 5
#define AGE_SECONDS 60
#define AGE_CHECK_SECONDS 5

#define PORT_FREE 0
#define PORT_USED 1
#define PORT_DONE 2

struct port {
    int id;
    int fd;
    int state;
    pthread_t rx;
    pthread_t tx;
    struct frame *queue[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    int open;
    long frames_in;
    long frames_out;
    long drops;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
};

struct port *ports;
int num_ports;
struct mac_table table;
sem_t free_ports;
pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;

int stop = 0;
pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;

int enqueue(struct port *p, struct frame *f)
{
    pthread_mutex_lock(&p->lock);
    if (!p->open) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    if (p->count == QUEUE_SIZE) {
        p->drops++;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->queue[p->tail] = f;
    p->tail = (p->tail + 1) % QUEUE_SIZE;
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void send_to(struct port *p, struct frame *f)
{
    frame_get(f);
    if (enqueue(p, f) < 0)
        frame_put(f);
}

void forward(struct port *in, struct frame *f)
{
    int out = -1;

    if (!is_broadcast(f->h.dst))
        out = mac_table_lookup(&table, f->h.dst);

    if (out < 0) {
        for (int i = 0; i < num_ports; i++)
            if (i != in->id)
                send_to(&ports[i], f);
    } else if (out != in->id) {
        send_to(&ports[out], f);
    }
}

void *tx_thread(void *arg)
{
    struct port *p = arg;
    int dead = 0;

    while (1) {
        pthread_mutex_lock(&p->lock);
        while (p->count == 0 && p->open)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        struct frame *f = p->queue[p->head];
        p->head = (p->head + 1) % QUEUE_SIZE;
        p->count--;
        pthread_mutex_unlock(&p->lock);

        if (!dead) {
            if (frame_send(p->fd, f) < 0) {
                dead = 1;
            } else {
                pthread_mutex_lock(&p->lock);
                p->frames_out++;
                pthread_mutex_unlock(&p->lock);
            }
        }
        frame_put(f);
    }
    return NULL;
}

void close_port(struct port *p)
{
    pthread_mutex_lock(&p->lock);
    p->open = 0;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);

    shutdown(p->fd, SHUT_RDWR);
    pthread_join(p->tx, NULL);
    mac_table_remove_port(&table, p->id);
    close(p->fd);
    printf("port %d down\n", p->id);

    pthread_mutex_lock(&ports_lock);
    p->state = PORT_DONE;
    pthread_mutex_unlock(&ports_lock);
    sem_post(&free_ports);
}

void *rx_thread(void *arg)
{
    struct port *p = arg;
    char str[18];

    while (1) {
        struct frame *f = frame_new();
        if (f == NULL)
            break;
        if (frame_recv(p->fd, f) < 0) {
            frame_put(f);
            break;
        }

        pthread_mutex_lock(&p->lock);
        p->frames_in++;
        pthread_mutex_unlock(&p->lock);

        int r = mac_table_insert(&table, f->h.src, p->id);
        if (r > 0 && mac_table_count_port(&table, p->id) > PORT_MAX_MACS) {
            mac_to_str(f->h.src, str);
            printf("port %d: too many MACs (%s), shutting port down\n", p->id, str);
            frame_put(f);
            break;
        }

        forward(p, f);
        frame_put(f);
    }

    close_port(p);
    return NULL;
}

void *aging_thread(void *arg)
{
    struct timespec ts;

    (void)arg;
    pthread_mutex_lock(&stop_lock);
    while (!stop) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += AGE_CHECK_SECONDS;
        pthread_cond_timedwait(&stop_cond, &stop_lock, &ts);
        if (stop)
            break;
        pthread_mutex_unlock(&stop_lock);
        mac_table_age(&table, AGE_SECONDS);
        pthread_mutex_lock(&stop_lock);
    }
    pthread_mutex_unlock(&stop_lock);
    return NULL;
}

int wait_for_port(void)
{
    struct timespec ts;
    int r;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    ts.tv_sec += WAIT_SECONDS;
    while ((r = sem_timedwait(&free_ports, &ts)) < 0 && errno == EINTR)
        ;
    return r;
}

void start_port(int fd)
{
    int i;
    int old;

    pthread_mutex_lock(&ports_lock);
    for (i = 0; i < num_ports; i++)
        if (ports[i].state != PORT_USED)
            break;
    old = ports[i].state;
    ports[i].state = PORT_USED;
    pthread_mutex_unlock(&ports_lock);

    if (old == PORT_DONE)
        pthread_join(ports[i].rx, NULL);

    struct port *p = &ports[i];
    p->fd = fd;
    pthread_mutex_lock(&p->lock);
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->open = 1;
    pthread_mutex_unlock(&p->lock);

    if (pthread_create(&p->tx, NULL, tx_thread, p) != 0 ||
        pthread_create(&p->rx, NULL, rx_thread, p) != 0) {
        printf("pthread_create failed\n");
        exit(1);
    }
    printf("host connected on port %d\n", i);
}

int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    pthread_t ager;
    int server;
    int opt = 1;

    if (argc != 3) {
        printf("usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return 1;
    }
    num_ports = atoi(argv[2]);
    if (num_ports <= 0) {
        printf("num_ports must be > 0\n");
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    ports = calloc(num_ports, sizeof(struct port));
    if (ports == NULL) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < num_ports; i++) {
        ports[i].id = i;
        ports[i].state = PORT_FREE;
        if (pthread_mutex_init(&ports[i].lock, NULL) != 0 ||
            pthread_cond_init(&ports[i].not_empty, NULL) != 0) {
            printf("init failed\n");
            return 1;
        }
    }
    if (mac_table_init(&table, MAX_ENTRIES) != 0) {
        printf("mac table init failed\n");
        return 1;
    }
    if (sem_init(&free_ports, 0, num_ports) < 0) {
        perror("sem_init");
        return 1;
    }
    if (pthread_create(&ager, NULL, aging_thread, NULL) != 0) {
        printf("pthread_create failed\n");
        return 1;
    }

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(atoi(argv[1]));
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server, 16) < 0) {
        perror("listen");
        return 1;
    }
    printf("switch listening on port %s with %d ports\n", argv[1], num_ports);

    while (1) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        if (wait_for_port() < 0) {
            char msg[] = "switch full\n";
            if (write_full(fd, msg, strlen(msg)) < 0)
                printf("could not send switch full\n");
            close(fd);
            printf("switch full, host rejected\n");
            continue;
        }
        start_port(fd);
    }

    return 0;
}
