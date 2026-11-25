#define _POSIX_C_SOURCE 200809L
#include "client.h"
#include "io.h"
#include "sim_clock.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t tcp_interrupted;
static void interrupted(int sig) { tcp_interrupted = sig; }
int tcp_signals(void) {
    struct sigaction action = { .sa_handler = interrupted };
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ? -1 : 0;
}
void client_print(const char *text) { write_all(STDOUT_FILENO, text, strlen(text)); }
int text_numbers(const char *text, long long *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        while (*text == ' ') text++;
        // Протокол использует только десятичные числа и одиночные разделители.
        if (!*text || (*text != '-' && (*text < '0' || *text > '9'))) { errno = EPROTO; return -1; }
        char *end; errno = 0;
        values[i] = strtoll(text, &end, 10);
        if (errno || end == text || (*end && *end != ' ')) { errno = EPROTO; return -1; }
        text = end;
    }
    while (*text == ' ') text++;
    if (*text) { errno = EPROTO; return -1; }
    return 0;
}
int client_send(Client *c, uint32_t type, const char *body) {
    if (c->seq == UINT32_MAX) { errno = EOVERFLOW; return -1; }
    return net_queue(&c->connection, type, ++c->seq, body);
}
void client_ack_stop(Client *c) {
    if (!c->stopped || c->stop_ack_sent) return;
    c->stop_ack_sent = true;
    int rc = client_send(c, NET_ACK, "");
    long long end = monotonic_ns() + 500000000LL;
    while (rc == 0 && net_pending(&c->connection) && monotonic_ns() < end) {
        rc = net_flush(&c->connection);
        if (net_pending(&c->connection)) {
            struct pollfd p = { .fd = c->connection.fd, .events = POLLOUT };
            if (poll(&p, 1, 10) < 0 && errno != EINTR) break;
        }
    }
}
static int finish_frame(Client *c, NetFrame *f) {
    if (f->type == NET_PONG) { net_frame_free(f); return 0; }
    if (f->type == NET_ERROR) {
        client_print("Ошибка сервера: "); client_print(f->body); client_print("\n");
        net_frame_free(f); errno = EPROTO; return -1;
    }
    if (f->type != NET_STOP) return 1;
    c->stopped = true;
    snprintf(c->summary, sizeof(c->summary), "%s", f->body);
    client_print(c->summary);
    net_frame_free(f);
    if (!c->defer_stop_ack) client_ack_stop(c);
    return 2;
}
int client_next(Client *c, NetFrame *f, int wait_ms) {
    int rc = net_next(&c->connection, f);
    if (rc != 0) return rc < 0 ? -1 : finish_frame(c, f);
    long long now = monotonic_ns();
    if (now - c->connection.last_input > NET_TIMEOUT_NS) { errno = ETIMEDOUT; return -1; }
    if (now >= c->heartbeat) {
        if (client_send(c, NET_PING, "") < 0) return -1;
        c->heartbeat = now + 1000000000LL;
    }
    if (net_flush(&c->connection) < 0) return -1;
    struct pollfd p = { .fd = c->connection.fd, .events = POLLIN | (net_pending(&c->connection) ? POLLOUT : 0) };
    rc = poll(&p, 1, wait_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (p.revents & POLLIN) {
        if (net_read(&c->connection) < 0) return -1;
    } else if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ECONNRESET; return -1; }
    if ((p.revents & POLLOUT) && net_flush(&c->connection) < 0) return -1;
    rc = net_next(&c->connection, f);
    return rc < 0 ? -1 : rc == 0 ? 0 : finish_frame(c, f);
}
int client_open(Client *c, const char *host, const char *port, int role, int id) {
    *c = (Client){ .id = id };
    net_init(&c->connection, -1, NET_MAX_BODY);
    long long end = monotonic_ns() + NET_TIMEOUT_NS;
    int fd;
    do {
        if (tcp_interrupted) { errno = ECANCELED; return -1; }
        fd = net_connect(host, port, 200);
        if (fd >= 0) break;
        if (errno == EINVAL) return -1;
        sleep_until(monotonic_ns() + 20000000LL);
    } while (monotonic_ns() < end);
    if (fd < 0) return -1;
    net_init(&c->connection, fd, NET_MAX_BODY);
    char hello[64]; snprintf(hello, sizeof(hello), "%d %d", role, id);
    if (client_send(c, NET_HELLO, hello) < 0) goto fail;
    end = monotonic_ns() + NET_TIMEOUT_NS;
    while (!tcp_interrupted && monotonic_ns() < end) {
        NetFrame f;
        int rc = client_next(c, &f, 20);
        if (rc < 0) goto fail;
        if (rc == 2) { errno = ECANCELED; goto fail; }
        if (rc == 1) {
            long long v[6];
            bool valid = f.type == NET_WELCOME && f.seq == 1 && text_numbers(f.body, v, 6) == 0 &&
                v[0] == id && v[1] >= 0 && v[1] <= 2 && v[2] >= 1 && v[2] <= 2147483647 &&
                v[3] >= 0 && v[3] <= 4096 && v[5] > 0;
            net_frame_free(&f);
            if (!valid) { errno = EPROTO; goto fail; }
            c->type = (int)v[1]; c->tick_ms = (int)v[2]; c->total = (int)v[3]; c->session = v[5];
            return 0;
        }
    }
    errno = tcp_interrupted ? ECANCELED : ETIMEDOUT;
fail:
    { int saved = errno; client_close(c); errno = saved; return -1; }
}
void client_detach(Client *c) {
    if (c->stopped || c->connection.fd < 0 || client_send(c, NET_DETACH, "") < 0) return;
    long long end = monotonic_ns() + 500000000LL;
    while (monotonic_ns() < end) {
        NetFrame f;
        int rc = client_next(c, &f, 10);
        if (rc < 0 || rc == 2) break;
        if (rc == 1) {
            bool done = f.type == NET_ACK;
            net_frame_free(&f);
            if (done) break;
        }
    }
}
void client_close(Client *c) { net_close(&c->connection); }
