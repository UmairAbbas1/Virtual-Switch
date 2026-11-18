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
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "frame.h"
#include "mac_table.h"
#include "shm_stats.h"

#define QUEUE_SIZE 64
#define PORT_MAX_MACS 8
#define WAIT_SECONDS 5
#define AGE_SECONDS 60
#define AGE_CHECK_SECONDS 5
#define SOCK_PATH "/tmp/vswitch.sock"
#define DUMP_FILE "mac_dump.txt"
#define TEXT_SIZE (MAX_ENTRIES * 48 + 128)

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
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
};

struct port *ports;
int num_ports;
struct mac_table table;
struct shm_stats *stats;
sem_t free_ports;
pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
int server_fd = -1;
int admin_fd = -1;

int stop = 0;
pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;

int stopping(void)
{
    int s;
    pthread_mutex_lock(&stop_lock);
    s = stop;
    pthread_mutex_unlock(&stop_lock);
    return s;
}

int send_text(int fd, char *text)
{
    return write_full(fd, text, strlen(text));
}

int enqueue(struct port *p, struct frame *f)
{
    pthread_mutex_lock(&p->lock);
    if (!p->open) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    if (p->count == QUEUE_SIZE) {
        pthread_mutex_unlock(&p->lock);
        stats_add(stats, p->id, STAT_DROP);
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
            if (frame_send(p->fd, f) < 0)
                dead = 1;
            else
                stats_add(stats, p->id, STAT_OUT);
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

    pthread_mutex_lock(&p->lock);
    close(p->fd);
    p->fd = -1;
    pthread_mutex_unlock(&p->lock);
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
        stats_add(stats, p->id, STAT_IN);

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

void show_mac(int fd)
{
    char *buf = malloc(TEXT_SIZE);
    if (buf == NULL) {
        send_text(fd, "out of memory\n");
        return;
    }
    mac_table_to_text(&table, buf, TEXT_SIZE);
    send_text(fd, buf);
    free(buf);
}

void show_ports(int fd)
{
    struct port_stats ps[MAX_PORTS];
    char line[200];
    int state;
    int queued;

    stats_read(stats, ps);
    send_text(fd, "PORT  STATE  QUEUE  IN        OUT       DROPPED   MIRRORED\n");
    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_lock(&ports_lock);
        state = ports[i].state;
        pthread_mutex_unlock(&ports_lock);

        pthread_mutex_lock(&ports[i].lock);
        queued = ports[i].count;
        pthread_mutex_unlock(&ports[i].lock);

        snprintf(line, sizeof(line), "%-4d  %-5s  %-5d  %-8ld  %-8ld  %-8ld  %-8ld\n", i,
                 state == PORT_USED ? "up" : "free", queued,
                 ps[i].in, ps[i].out, ps[i].dropped, ps[i].mirrored);
        send_text(fd, line);
    }
}

void handle_command(int fd, char *cmd)
{
    char line[100];

    printf("admin command: %s\n", cmd);
    if (strcmp(cmd, "show mac") == 0) {
        show_mac(fd);
    } else if (strcmp(cmd, "show ports") == 0 || strcmp(cmd, "stats") == 0) {
        show_ports(fd);
    } else if (strcmp(cmd, "clear mac") == 0) {
        int n = mac_table_clear(&table);
        snprintf(line, sizeof(line), "MAC table cleared (%d entries removed)\n", n);
        send_text(fd, line);
    } else {
        send_text(fd, "unknown command\n");
    }
}

int read_line(int fd, char *buf, int size)
{
    int n = 0;
    char c;

    while (n < size - 1) {
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

void *admin_thread(void *arg)
{
    char cmd[256];

    (void)arg;
    while (1) {
        int fd = accept(admin_fd, NULL, NULL);
        if (fd < 0) {
            if (stopping())
                break;
            if (errno != EINTR)
                perror("admin accept");
            continue;
        }
        if (read_line(fd, cmd, sizeof(cmd)) > 0)
            handle_command(fd, cmd);
        close(fd);
    }
    return NULL;
}

void dump_mac_table(void)
{
    time_t now = time(NULL);
    char *buf = malloc(TEXT_SIZE);
    if (buf == NULL) {
        printf("dump failed: out of memory\n");
        return;
    }
    mac_table_to_text(&table, buf, TEXT_SIZE);

    FILE *f = fopen(DUMP_FILE, "w");
    if (f == NULL) {
        perror("fopen");
        free(buf);
        return;
    }
    fprintf(f, "MAC table dump at %s", ctime(&now));
    fputs(buf, f);
    if (fclose(f) != 0)
        perror("fclose");
    else
        printf("MAC table dumped to %s\n", DUMP_FILE);
    free(buf);
}

void begin_shutdown(void)
{
    pthread_mutex_lock(&stop_lock);
    stop = 1;
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_lock);

    shutdown(server_fd, SHUT_RDWR);
    shutdown(admin_fd, SHUT_RDWR);
    sem_post(&free_ports);
}

void *signal_thread(void *arg)
{
    sigset_t *set = arg;
    int sig;

    while (1) {
        if (sigwait(set, &sig) != 0)
            continue;
        if (sig == SIGUSR1) {
            dump_mac_table();
        } else if (sig == SIGHUP) {
            printf("SIGHUP received: config reload\n");
        } else {
            printf("signal %d received, shutting down\n", sig);
            begin_shutdown();
            break;
        }
    }
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
    pthread_mutex_lock(&p->lock);
    p->fd = fd;
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

int make_server(int tcp_port)
{
    struct sockaddr_in addr;
    int opt = 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(tcp_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

int make_admin_socket(void)
{
    struct sockaddr_un addr;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("admin socket");
        return -1;
    }
    unlink(SOCK_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        perror("admin bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    pthread_t ager, admin, sig_th;
    sigset_t set;

    if (argc != 3) {
        printf("usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return 1;
    }
    num_ports = atoi(argv[2]);
    if (num_ports <= 0 || num_ports > MAX_PORTS) {
        printf("num_ports must be 1 to %d\n", MAX_PORTS);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        printf("pthread_sigmask failed\n");
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    ports = calloc(num_ports, sizeof(struct port));
    if (ports == NULL) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < num_ports; i++) {
        ports[i].id = i;
        ports[i].fd = -1;
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
    stats = stats_create(num_ports);
    if (stats == NULL)
        return 1;

    server_fd = make_server(atoi(argv[1]));
    admin_fd = make_admin_socket();
    if (server_fd < 0 || admin_fd < 0)
        return 1;

    if (pthread_create(&sig_th, NULL, signal_thread, &set) != 0 ||
        pthread_create(&ager, NULL, aging_thread, NULL) != 0 ||
        pthread_create(&admin, NULL, admin_thread, NULL) != 0) {
        printf("pthread_create failed\n");
        return 1;
    }
    printf("switch listening on port %s with %d ports, admin socket %s\n",
           argv[1], num_ports, SOCK_PATH);

    while (!stopping()) {
        int fd = accept(server_fd, NULL, NULL);
        if (fd < 0) {
            if (!stopping() && errno != EINTR)
                perror("accept");
            continue;
        }
        if (wait_for_port() < 0) {
            if (send_text(fd, "switch full\n") < 0)
                printf("could not send switch full\n");
            close(fd);
            printf("switch full, host rejected\n");
            continue;
        }
        if (stopping()) {
            close(fd);
            break;
        }
        start_port(fd);
    }

    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_lock(&ports[i].lock);
        if (ports[i].fd >= 0)
            shutdown(ports[i].fd, SHUT_RDWR);
        pthread_mutex_unlock(&ports[i].lock);
    }
    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_lock(&ports_lock);
        int state = ports[i].state;
        ports[i].state = PORT_FREE;
        pthread_mutex_unlock(&ports_lock);
        if (state != PORT_FREE)
            pthread_join(ports[i].rx, NULL);
    }
    pthread_join(ager, NULL);
    pthread_join(admin, NULL);
    pthread_join(sig_th, NULL);

    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_destroy(&ports[i].lock);
        pthread_cond_destroy(&ports[i].not_empty);
    }
    free(ports);
    mac_table_destroy(&table);
    sem_destroy(&free_ports);
    close(server_fd);
    close(admin_fd);
    unlink(SOCK_PATH);
    stats_destroy(stats);
    printf("switch stopped cleanly\n");
    return 0;
}
