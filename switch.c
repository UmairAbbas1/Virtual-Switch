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
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "frame.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "logger.h"
#include "config.h"

/*
 * Lock order (always take locks in this order, never backwards):
 *   1. ports_lock
 *   2. ports[i].lock, in ascending port number when more than one is held
 *   3. MAC table bucket locks in ascending bucket number, then the table count_lock
 *   4. frame lock (reference count)
 * cfg_lock, stop_lock, the shared memory stats lock and the logger lock are
 * leaf locks: nothing else is taken while one of them is held.
 */

#define QUEUE_SIZE 64
#define AGE_CHECK_SECONDS 5
#define TICK_MS 100
#define TEXT_SIZE 65536

#define PORT_FREE 0
#define PORT_USED 1
#define PORT_DOWN 2

#define Q_OK 0
#define Q_SKIP -1
#define Q_DROP -2

struct qitem {
    struct frame *f;
    struct timespec t;
    int mirror_copy;
};

struct fqueue {
    struct qitem items[QUEUE_SIZE];
    int head;
    int tail;
    int count;
};

struct port {
    int id;
    pthread_t rx;
    pthread_t tx;

    /* protected by ports_lock */
    int state;
    int need_join;
    int want_down;

    /* protected by lock */
    int fd;
    int wfd;
    int wake[2];
    int open;
    struct fqueue q[3];
    int total;
    int weights[3];
    int wrr_cur;
    int wrr_left;
    int aged_this_round;
    int max_wait_ms;
    int rate;
    int tokens;
    int vlan;
    int trunk;
    int mirror_to;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t can_send;
};

struct port *ports;
int num_ports;
struct mac_table table;
struct shm_stats *stats;
sem_t free_ports;
pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
int server_fd = -1;
int admin_fd = -1;
char *config_path;
char sock_path[108] = "/tmp/vswitch.sock";
char stats_name[64] = "/vswitch_stats";
char mac_name[64] = "/vswitch_mac";
char dump_file[100] = "mac_dump.txt";
int link_port = -1;

struct config cfg;
pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;

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

int setting(int *field)
{
    int v;
    pthread_mutex_lock(&cfg_lock);
    v = *field;
    pthread_mutex_unlock(&cfg_lock);
    return v;
}

int send_text(int fd, char *text)
{
    return write_full(fd, text, strlen(text));
}

void deadline_ms(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

long ms_between(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

/*
 * Wake the rx thread of a port so it stops reading (p->lock held).
 * A socket is shut down; a pipe cannot be, so a byte goes on its wake pipe.
 */
void kick_port(struct port *p)
{
    if (p->wake[1] >= 0) {
        if (write(p->wake[1], "x", 1) < 0)
            log_msg("port %d: wake write failed: %s", p->id, strerror(errno));
    } else if (p->fd >= 0) {
        shutdown(p->fd, SHUT_RDWR);
    }
}

/* ---------- queues ---------- */

/*
 * Put f on port p. vlan > 0 means only queue it if p is a trunk or is in that
 * VLAN; vlan 0 (mirror copies) skips the check. Never blocks: a full queue
 * drops the frame and counts the drop.
 */
int enqueue(struct port *p, struct frame *f, int vlan, int mirror_copy)
{
    int prio = f->h.priority > 2 ? 2 : f->h.priority;

    pthread_mutex_lock(&p->lock);
    if (!p->open || (vlan > 0 && !p->trunk && p->vlan != vlan)) {
        pthread_mutex_unlock(&p->lock);
        return Q_SKIP;
    }
    struct fqueue *q = &p->q[prio];
    if (q->count == QUEUE_SIZE) {
        pthread_mutex_unlock(&p->lock);
        stats_add(stats, p->id, STAT_DROP);
        return Q_DROP;
    }
    frame_get(f);
    struct qitem *it = &q->items[q->tail];
    it->f = f;
    it->mirror_copy = mirror_copy;
    clock_gettime(CLOCK_MONOTONIC, &it->t);
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    p->total++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return Q_OK;
}

struct qitem pop(struct port *p, int c)
{
    struct fqueue *q = &p->q[c];
    struct qitem it = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    p->total--;
    return it;
}

/*
 * Choose the next frame (p->lock held, p->total > 0).
 * First aging: a low or normal frame that waited max_wait_ms goes now, but
 * only once per round, so under heavy load aging cannot undo the priorities.
 * Otherwise weighted round robin: up to weights[c] frames from class c,
 * then move to the next class.
 */
struct qitem pick(struct port *p)
{
    if (p->max_wait_ms > 0 && !p->aged_this_round) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (int c = 2; c >= 1; c--) {
            struct fqueue *q = &p->q[c];
            if (q->count > 0 && ms_between(&q->items[q->head].t, &now) >= p->max_wait_ms) {
                p->aged_this_round = 1;
                return pop(p, c);
            }
        }
    }
    while (1) {
        if (p->wrr_left > 0 && p->q[p->wrr_cur].count > 0) {
            p->wrr_left--;
            return pop(p, p->wrr_cur);
        }
        p->wrr_cur = (p->wrr_cur + 1) % 3;
        p->wrr_left = p->weights[p->wrr_cur];
        if (p->wrr_cur == 0)
            p->aged_this_round = 0;
    }
}

void forward(struct port *in, struct frame *f)
{
    int vlan = f->h.vlan;
    int out = -1;

    if (!is_broadcast(f->h.dst))
        out = mac_table_lookup(&table, f->h.dst);
    if (out == in->id)
        return;
    if (out >= 0 && enqueue(&ports[out], f, vlan, 0) != Q_SKIP)
        return;

    for (int i = 0; i < num_ports; i++)
        if (i != in->id)
            enqueue(&ports[i], f, vlan, 0);
}

/* ---------- port threads ---------- */

void *tx_thread(void *arg)
{
    struct port *p = arg;
    struct timespec ts;
    struct qitem it;
    int dead = 0;

    pthread_mutex_lock(&p->lock);
    while (1) {
        while (p->total == 0 && p->open)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (!p->open)
            break;
        if (p->rate > 0 && p->tokens < 10) {
            deadline_ms(&ts, 1000);
            pthread_cond_timedwait(&p->can_send, &p->lock, &ts);
            continue;
        }
        it = pick(p);
        if (p->rate > 0)
            p->tokens -= 10;
        int fd = p->wfd;
        int m = p->mirror_to;
        pthread_mutex_unlock(&p->lock);

        if (!dead) {
            if (frame_send(fd, it.f) < 0) {
                dead = 1;
            } else {
                stats_add(stats, p->id, STAT_OUT);
                if (m >= 0 && m != p->id && !it.mirror_copy &&
                    enqueue(&ports[m], it.f, 0, 1) == Q_OK)
                    stats_add(stats, p->id, STAT_MIRROR);
            }
        }
        frame_put(it.f);
        pthread_mutex_lock(&p->lock);
    }

    /* port closed: drain whatever is left */
    for (int c = 0; c < 3; c++)
        while (p->q[c].count > 0)
            frame_put(pop(p, c).f);
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

void close_port(struct port *p)
{
    int down;

    pthread_mutex_lock(&p->lock);
    p->open = 0;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->can_send);
    kick_port(p);
    pthread_mutex_unlock(&p->lock);

    pthread_join(p->tx, NULL);
    mac_table_remove_port(&table, p->id);

    pthread_mutex_lock(&p->lock);
    close(p->fd);
    if (p->wfd != p->fd)
        close(p->wfd);
    if (p->wake[0] >= 0) {
        close(p->wake[0]);
        close(p->wake[1]);
    }
    p->fd = -1;
    p->wfd = -1;
    p->wake[0] = -1;
    p->wake[1] = -1;
    pthread_mutex_unlock(&p->lock);

    pthread_mutex_lock(&ports_lock);
    down = p->want_down;
    p->want_down = 0;
    p->state = down ? PORT_DOWN : PORT_FREE;
    pthread_mutex_unlock(&ports_lock);

    if (down) {
        log_msg("port %d shut down", p->id);
    } else {
        log_msg("port %d disconnected", p->id);
        sem_post(&free_ports);
    }
}

void *rx_thread(void *arg)
{
    struct port *p = arg;
    char str[18];
    int violation = 0;

    pthread_mutex_lock(&p->lock);
    int in_fd = p->fd;
    int wake_fd = p->wake[0];
    pthread_mutex_unlock(&p->lock);

    while (1) {
        struct frame *f = frame_new();
        if (f == NULL)
            break;
        if (frame_recv_wake(in_fd, wake_fd, f) < 0) {
            frame_put(f);
            break;
        }
        stats_add(stats, p->id, STAT_IN);

        pthread_mutex_lock(&p->lock);
        int vlan = p->vlan;
        int trunk = p->trunk;
        int m = p->mirror_to;
        pthread_mutex_unlock(&p->lock);

        if (trunk) {
            if (f->h.vlan < 1 || f->h.vlan > 4094) {
                frame_put(f);
                continue;
            }
        } else {
            f->h.vlan = vlan;
        }

        int r = mac_table_insert(&table, f->h.src, p->id);
        if (!trunk && r > 0 &&
            mac_table_count_port(&table, p->id) > setting(&cfg.port_max_macs)) {
            mac_to_str(f->h.src, str);
            log_msg("port %d: too many MACs (%s), port security shuts it down", p->id, str);
            violation = 1;
            frame_put(f);
            break;
        }

        if (m >= 0 && m != p->id && enqueue(&ports[m], f, 0, 1) == Q_OK)
            stats_add(stats, p->id, STAT_MIRROR);
        forward(p, f);
        frame_put(f);
    }

    if (violation) {
        pthread_mutex_lock(&ports_lock);
        p->want_down = 1;
        pthread_mutex_unlock(&ports_lock);
    }
    close_port(p);
    return NULL;
}

/* ---------- background threads ---------- */

void *aging_thread(void *arg)
{
    struct timespec ts;

    (void)arg;
    pthread_mutex_lock(&stop_lock);
    while (!stop) {
        deadline_ms(&ts, AGE_CHECK_SECONDS * 1000);
        pthread_cond_timedwait(&stop_cond, &stop_lock, &ts);
        if (stop)
            break;
        pthread_mutex_unlock(&stop_lock);
        mac_table_age(&table, setting(&cfg.aging));
        pthread_mutex_lock(&stop_lock);
    }
    pthread_mutex_unlock(&stop_lock);
    return NULL;
}

/* token bucket refill: every TICK_MS add rate/10 frames (tokens are in tenths) */
void *timer_thread(void *arg)
{
    struct timespec ts;

    (void)arg;
    pthread_mutex_lock(&stop_lock);
    while (!stop) {
        deadline_ms(&ts, TICK_MS);
        pthread_cond_timedwait(&stop_cond, &stop_lock, &ts);
        if (stop)
            break;
        pthread_mutex_unlock(&stop_lock);

        for (int i = 0; i < num_ports; i++) {
            struct port *p = &ports[i];
            pthread_mutex_lock(&p->lock);
            if (p->rate > 0) {
                p->tokens += p->rate;
                if (p->tokens > p->rate * 10)
                    p->tokens = p->rate * 10;
                pthread_cond_broadcast(&p->can_send);
            }
            pthread_mutex_unlock(&p->lock);
        }
        pthread_mutex_lock(&stop_lock);
    }
    pthread_mutex_unlock(&stop_lock);
    return NULL;
}

void lock_all_ports(void)
{
    for (int i = 0; i < num_ports; i++)
        pthread_mutex_lock(&ports[i].lock);
}

void unlock_all_ports(void)
{
    for (int i = num_ports - 1; i >= 0; i--)
        pthread_mutex_unlock(&ports[i].lock);
}

/* apply rate, weights, VLANs and trunk from c to every port */
void apply_config(struct config *c)
{
    int changed[MAX_PORTS];

    pthread_mutex_lock(&cfg_lock);
    int table_size = cfg.mac_table_size;
    char log_file[sizeof(cfg.log_file)];
    strcpy(log_file, cfg.log_file);
    cfg = *c;
    cfg.mac_table_size = table_size;
    strcpy(cfg.log_file, log_file);
    pthread_mutex_unlock(&cfg_lock);

    lock_all_ports();
    for (int i = 0; i < num_ports; i++) {
        struct port *p = &ports[i];
        int trunk = (i == (link_port >= 0 ? link_port : c->trunk));
        changed[i] = (p->vlan != c->vlan[i] || p->trunk != trunk);
        p->vlan = c->vlan[i];
        p->trunk = trunk;
        p->rate = c->rate;
        if (p->tokens > p->rate * 10)
            p->tokens = p->rate * 10;
        for (int k = 0; k < 3; k++)
            p->weights[k] = c->weights[k];
        p->max_wait_ms = c->max_wait_ms;
        pthread_cond_broadcast(&p->can_send);
    }
    unlock_all_ports();

    for (int i = 0; i < num_ports; i++)
        if (changed[i])
            mac_table_remove_port(&table, i);
}

/* ---------- admin commands ---------- */

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
    char line[256];
    char mirror[16];

    stats_read(stats, ps);
    send_text(fd, "PORT  STATE  VLAN  MODE    RATE   MIRROR  QUEUE  "
                  "IN        OUT       DROPPED   MIRRORED\n");
    for (int i = 0; i < num_ports; i++) {
        struct port *p = &ports[i];

        pthread_mutex_lock(&ports_lock);
        int state = p->state;
        pthread_mutex_unlock(&ports_lock);

        pthread_mutex_lock(&p->lock);
        int vlan = p->vlan;
        int trunk = p->trunk;
        int rate = p->rate;
        int m = p->mirror_to;
        int queued = p->total;
        pthread_mutex_unlock(&p->lock);

        if (m >= 0)
            snprintf(mirror, sizeof(mirror), "->%d", m);
        else
            strcpy(mirror, "-");
        snprintf(line, sizeof(line),
                 "%-4d  %-5s  %-4d  %-6s  %-5d  %-6s  %-5d  %-8ld  %-8ld  %-8ld  %-8ld\n", i,
                 state == PORT_USED ? "up" : state == PORT_FREE ? "free" : "down",
                 vlan, trunk ? "trunk" : "access", rate, mirror, queued,
                 ps[i].in, ps[i].out, ps[i].dropped, ps[i].mirrored);
        send_text(fd, line);
    }
}

void port_shutdown(int fd, int i)
{
    struct port *p = &ports[i];
    char line[100];
    int join = 0;

    pthread_mutex_lock(&ports_lock);
    if (p->state == PORT_DOWN) {
        pthread_mutex_unlock(&ports_lock);
        send_text(fd, "port is already down\n");
        return;
    }
    if (p->state == PORT_FREE) {
        if (sem_trywait(&free_ports) < 0) {
            pthread_mutex_unlock(&ports_lock);
            send_text(fd, "port is being taken by a new host, try again\n");
            return;
        }
        if (p->need_join) {
            pthread_join(p->rx, NULL);
            p->need_join = 0;
        }
        p->state = PORT_DOWN;
        pthread_mutex_unlock(&ports_lock);
        log_msg("port %d shut down by admin (no host)", i);
    } else {
        /* a host is connected: kick it, rx thread closes the port as DOWN */
        p->want_down = 1;
        join = p->need_join;
        p->need_join = 0;
        pthread_mutex_lock(&p->lock);
        kick_port(p);
        pthread_mutex_unlock(&p->lock);
        pthread_mutex_unlock(&ports_lock);
        if (join)
            pthread_join(p->rx, NULL);
        log_msg("port %d shut down by admin", i);
    }
    snprintf(line, sizeof(line), "port %d is down\n", i);
    send_text(fd, line);
}

void port_up(int fd, int i)
{
    struct port *p = &ports[i];
    char line[100];

    pthread_mutex_lock(&ports_lock);
    if (p->state != PORT_DOWN) {
        pthread_mutex_unlock(&ports_lock);
        send_text(fd, "port is already up\n");
        return;
    }
    if (p->need_join) {
        pthread_join(p->rx, NULL);
        p->need_join = 0;
    }
    p->state = PORT_FREE;
    pthread_mutex_unlock(&ports_lock);
    sem_post(&free_ports);

    log_msg("port %d brought up by admin", i);
    snprintf(line, sizeof(line), "port %d is up\n", i);
    send_text(fd, line);
}

void set_vlan(int fd, int i, int vlan)
{
    char line[100];

    pthread_mutex_lock(&ports[i].lock);
    int old = ports[i].vlan;
    ports[i].vlan = vlan;
    pthread_mutex_unlock(&ports[i].lock);

    if (old != vlan)
        mac_table_remove_port(&table, i);
    log_msg("port %d moved from VLAN %d to VLAN %d", i, old, vlan);
    snprintf(line, sizeof(line), "port %d is in VLAN %d\n", i, vlan);
    send_text(fd, line);
}

void set_trunk(int fd, int t)
{
    char line[100];
    int changed[MAX_PORTS];

    lock_all_ports();
    for (int i = 0; i < num_ports; i++) {
        changed[i] = (ports[i].trunk != (i == t));
        ports[i].trunk = (i == t);
    }
    unlock_all_ports();

    for (int i = 0; i < num_ports; i++)
        if (changed[i])
            mac_table_remove_port(&table, i);
    if (t < 0) {
        log_msg("no trunk port");
        send_text(fd, "no trunk port\n");
    } else {
        log_msg("port %d is now the trunk", t);
        snprintf(line, sizeof(line), "port %d is the trunk\n", t);
        send_text(fd, line);
    }
}

void set_mirror(int fd, int i, int to)
{
    char line[100];

    pthread_mutex_lock(&ports[i].lock);
    ports[i].mirror_to = to;
    pthread_mutex_unlock(&ports[i].lock);

    if (to < 0) {
        log_msg("mirroring of port %d stopped", i);
        snprintf(line, sizeof(line), "port %d not mirrored\n", i);
    } else {
        log_msg("port %d mirrored to port %d", i, to);
        snprintf(line, sizeof(line), "port %d mirrored to port %d\n", i, to);
    }
    send_text(fd, line);
}

void set_rate(int fd, int i, int rate)
{
    char line[100];

    pthread_mutex_lock(&ports[i].lock);
    ports[i].rate = rate;
    if (ports[i].tokens > rate * 10)
        ports[i].tokens = rate * 10;
    pthread_cond_broadcast(&ports[i].can_send);
    pthread_mutex_unlock(&ports[i].lock);

    log_msg("port %d rate set to %d frames/s", i, rate);
    snprintf(line, sizeof(line), "port %d rate %d frames/s%s\n", i, rate,
             rate == 0 ? " (unlimited)" : "");
    send_text(fd, line);
}

int valid_port(int fd, int i)
{
    if (i < 0 || i >= num_ports) {
        send_text(fd, "bad port number\n");
        return 0;
    }
    return 1;
}

void handle_command(int fd, char *cmd)
{
    char word[16];
    int a, b;

    log_msg("admin command: %s", cmd);
    if (strcmp(cmd, "show mac") == 0) {
        show_mac(fd);
    } else if (strcmp(cmd, "show ports") == 0 || strcmp(cmd, "stats") == 0) {
        show_ports(fd);
    } else if (strcmp(cmd, "clear mac") == 0) {
        char line[100];
        int n = mac_table_clear(&table);
        log_msg("MAC table cleared by admin (%d entries)", n);
        snprintf(line, sizeof(line), "MAC table cleared (%d entries removed)\n", n);
        send_text(fd, line);
    } else if (sscanf(cmd, "shutdown %d", &a) == 1) {
        if (valid_port(fd, a))
            port_shutdown(fd, a);
    } else if (sscanf(cmd, "up %d", &a) == 1) {
        if (valid_port(fd, a))
            port_up(fd, a);
    } else if (sscanf(cmd, "vlan %d %d", &a, &b) == 2) {
        if (valid_port(fd, a)) {
            if (b < 1 || b > 4094)
                send_text(fd, "VLAN must be 1 to 4094\n");
            else
                set_vlan(fd, a, b);
        }
    } else if (sscanf(cmd, "mirror %d %15s", &a, word) == 2) {
        if (!valid_port(fd, a))
            return;
        if (strcmp(word, "off") == 0) {
            set_mirror(fd, a, -1);
        } else if (sscanf(word, "%d", &b) == 1 && valid_port(fd, b)) {
            if (a == b)
                send_text(fd, "a port cannot mirror to itself\n");
            else
                set_mirror(fd, a, b);
        }
    } else if (sscanf(cmd, "trunk %15s", word) == 1) {
        if (strcmp(word, "off") == 0)
            set_trunk(fd, -1);
        else if (sscanf(word, "%d", &a) == 1 && valid_port(fd, a))
            set_trunk(fd, a);
    } else if (sscanf(cmd, "rate %d %d", &a, &b) == 2) {
        if (valid_port(fd, a)) {
            if (b < 0)
                send_text(fd, "rate must be 0 (unlimited) or more\n");
            else
                set_rate(fd, a, b);
        }
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
    struct timeval tv = {2, 0};

    (void)arg;
    while (1) {
        int fd = accept(admin_fd, NULL, NULL);
        if (fd < 0) {
            if (stopping())
                break;
            if (errno != EINTR)
                log_msg("admin accept failed: %s", strerror(errno));
            continue;
        }
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            log_msg("admin setsockopt failed: %s", strerror(errno));
        if (read_line(fd, cmd, sizeof(cmd)) > 0)
            handle_command(fd, cmd);
        close(fd);
    }
    return NULL;
}

/* ---------- signals ---------- */

void dump_mac_table(void)
{
    char when[64];
    struct tm tm;
    time_t now = time(NULL);

    char *buf = malloc(TEXT_SIZE);
    if (buf == NULL) {
        log_msg("MAC dump failed: out of memory");
        return;
    }
    mac_table_to_text(&table, buf, TEXT_SIZE);
    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);

    FILE *f = fopen(dump_file, "w");
    if (f == NULL) {
        log_msg("MAC dump failed: cannot open %s", dump_file);
        free(buf);
        return;
    }
    fprintf(f, "MAC table dump at %s\n", when);
    fputs(buf, f);
    if (fclose(f) != 0)
        log_msg("MAC dump failed: cannot write %s", dump_file);
    else
        log_msg("MAC table dumped to %s", dump_file);
    free(buf);
}

void reload_config(void)
{
    struct config c;

    if (config_path == NULL) {
        log_msg("SIGHUP: no config file given, nothing to reload");
        return;
    }
    if (config_load(config_path, &c, num_ports) < 0) {
        log_msg("SIGHUP: config not reloaded, keeping the old settings");
        return;
    }
    apply_config(&c);
    log_msg("SIGHUP: config reloaded from %s", config_path);
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
            reload_config();
        } else {
            log_msg("signal %d received, shutting down", sig);
            begin_shutdown();
            break;
        }
    }
    return NULL;
}

/* ---------- accepting hosts ---------- */

int wait_for_port(void)
{
    struct timespec ts;
    int r;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    ts.tv_sec += setting(&cfg.wait_seconds);
    while ((r = sem_timedwait(&free_ports, &ts)) < 0 && errno == EINTR)
        ;
    return r;
}

/* start the threads of port i (ports_lock held, port is free) */
void open_port(int i, int rfd, int wfd, int pipe_link)
{
    struct port *p = &ports[i];
    if (p->need_join)
        pthread_join(p->rx, NULL);
    p->state = PORT_USED;
    p->need_join = 1;

    pthread_mutex_lock(&p->lock);
    p->fd = rfd;
    p->wfd = wfd;
    if (pipe_link && pipe(p->wake) < 0) {
        log_msg("pipe failed: %s", strerror(errno));
        exit(1);
    }
    p->open = 1;
    p->wrr_cur = 0;
    p->wrr_left = p->weights[0];
    p->aged_this_round = 0;
    p->tokens = p->rate * 10;
    pthread_mutex_unlock(&p->lock);

    if (pthread_create(&p->tx, NULL, tx_thread, p) != 0 ||
        pthread_create(&p->rx, NULL, rx_thread, p) != 0) {
        log_msg("pthread_create failed");
        exit(1);
    }
}

void start_port(int fd)
{
    int i;

    pthread_mutex_lock(&ports_lock);
    for (i = 0; i < num_ports; i++)
        if (ports[i].state == PORT_FREE)
            break;
    if (i == num_ports) {
        pthread_mutex_unlock(&ports_lock);
        sem_post(&free_ports);
        send_text(fd, "switch full\n");
        close(fd);
        return;
    }
    open_port(i, fd, fd, 0);
    pthread_mutex_unlock(&ports_lock);
    log_msg("host connected on port %d", i);
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
    unlink(sock_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        perror("admin bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    pthread_t ager, timer, admin, sig_th;
    struct config start;
    sigset_t set;
    char *name = NULL;
    int link_rfd = -1, link_wfd = -1;
    int restore = 0;
    int opt;

    while ((opt = getopt(argc, argv, "n:l:r")) != -1) {
        if (opt == 'n') {
            name = optarg;
        } else if (opt == 'l') {
            if (sscanf(optarg, "%d,%d", &link_rfd, &link_wfd) != 2) {
                printf("-l needs <read_fd>,<write_fd>\n");
                return 1;
            }
        } else if (opt == 'r') {
            restore = 1;
        } else {
            argc = 0;
        }
    }
    argv += optind - 1;
    argc -= optind - 1;
    if (argc != 3 && argc != 4) {
        printf("usage: switch [-n name] [-l rfd,wfd] [-r] <tcp_port> <num_ports> [config_file]\n"
               "  -n name     run as a named switch (own admin socket, shared memory, log)\n"
               "  -l rfd,wfd  the last port is a trunk link over these pipe fds\n"
               "  -r          restore the MAC table from shared memory (used by supervisor)\n");
        return 1;
    }
    num_ports = atoi(argv[2]);
    if (num_ports <= 0 || num_ports > MAX_PORTS) {
        printf("num_ports must be 1 to %d\n", MAX_PORTS);
        return 1;
    }
    if (link_rfd >= 0) {
        if (num_ports < 2) {
            printf("a pipe link needs at least 2 ports\n");
            return 1;
        }
        link_port = num_ports - 1;
    }
    if (name != NULL) {
        snprintf(sock_path, sizeof(sock_path), "/tmp/vswitch-%s.sock", name);
        snprintf(stats_name, sizeof(stats_name), "/vswitch_stats-%s", name);
        snprintf(mac_name, sizeof(mac_name), "/vswitch_mac-%s", name);
        snprintf(dump_file, sizeof(dump_file), "mac_dump-%s.txt", name);
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

    config_default(&start);
    if (argc == 4) {
        config_path = argv[3];
        if (config_load(config_path, &start, num_ports) < 0)
            return 1;
    }
    if (name != NULL && strcmp(start.log_file, "switch.log") == 0)
        snprintf(start.log_file, sizeof(start.log_file), "switch-%s.log", name);
    cfg = start;
    if (log_start(cfg.log_file) < 0)
        return 1;

    ports = calloc(num_ports, sizeof(struct port));
    if (ports == NULL) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < num_ports; i++) {
        ports[i].id = i;
        ports[i].fd = -1;
        ports[i].wfd = -1;
        ports[i].wake[0] = -1;
        ports[i].wake[1] = -1;
        ports[i].state = PORT_FREE;
        ports[i].mirror_to = -1;
        /* start with the configured VLAN, or apply_config sees a change and
           flushes the MACs that were just restored from shared memory */
        ports[i].vlan = start.vlan[i];
        ports[i].trunk = (i == (link_port >= 0 ? link_port : start.trunk));
        if (pthread_mutex_init(&ports[i].lock, NULL) != 0 ||
            pthread_cond_init(&ports[i].not_empty, NULL) != 0 ||
            pthread_cond_init(&ports[i].can_send, NULL) != 0) {
            printf("init failed\n");
            return 1;
        }
    }
    if (mac_table_init(&table, cfg.mac_table_size, mac_name, restore, num_ports) != 0) {
        printf("mac table init failed\n");
        return 1;
    }
    apply_config(&start);
    if (sem_init(&free_ports, 0, num_ports - (link_port >= 0)) < 0) {
        perror("sem_init");
        return 1;
    }
    stats = stats_create(stats_name, num_ports);
    if (stats == NULL)
        return 1;

    server_fd = make_server(atoi(argv[1]));
    admin_fd = make_admin_socket();
    if (server_fd < 0 || admin_fd < 0)
        return 1;

    if (pthread_create(&sig_th, NULL, signal_thread, &set) != 0 ||
        pthread_create(&ager, NULL, aging_thread, NULL) != 0 ||
        pthread_create(&timer, NULL, timer_thread, NULL) != 0 ||
        pthread_create(&admin, NULL, admin_thread, NULL) != 0) {
        printf("pthread_create failed\n");
        return 1;
    }
    if (link_port >= 0) {
        pthread_mutex_lock(&ports_lock);
        open_port(link_port, link_rfd, link_wfd, 1);
        pthread_mutex_unlock(&ports_lock);
        log_msg("port %d is a trunk link over pipes (read fd %d, write fd %d)",
                link_port, link_rfd, link_wfd);
    }
    log_msg("switch listening on port %s with %d ports, admin socket %s, log %s",
            argv[1], num_ports, sock_path, cfg.log_file);

    while (!stopping()) {
        int fd = accept(server_fd, NULL, NULL);
        if (fd < 0) {
            if (!stopping() && errno != EINTR)
                log_msg("accept failed: %s", strerror(errno));
            continue;
        }
        if (wait_for_port() < 0) {
            if (send_text(fd, "switch full\n") < 0)
                log_msg("could not send switch full");
            close(fd);
            log_msg("switch full, host rejected");
            continue;
        }
        if (stopping()) {
            close(fd);
            break;
        }
        start_port(fd);
    }

    /* clean shutdown: kick every host, then join every thread */
    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_lock(&ports[i].lock);
        kick_port(&ports[i]);
        pthread_mutex_unlock(&ports[i].lock);
    }
    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_lock(&ports_lock);
        int join = ports[i].need_join;
        ports[i].need_join = 0;
        pthread_mutex_unlock(&ports_lock);
        if (join)
            pthread_join(ports[i].rx, NULL);
    }
    pthread_join(ager, NULL);
    pthread_join(timer, NULL);
    pthread_join(admin, NULL);
    pthread_join(sig_th, NULL);

    for (int i = 0; i < num_ports; i++) {
        pthread_mutex_destroy(&ports[i].lock);
        pthread_cond_destroy(&ports[i].not_empty);
        pthread_cond_destroy(&ports[i].can_send);
    }
    free(ports);
    mac_table_destroy(&table);
    sem_destroy(&free_ports);
    close(server_fd);
    close(admin_fd);
    unlink(sock_path);
    stats_destroy(stats, stats_name);
    log_msg("switch stopped cleanly");
    log_stop();
    return 0;
}
