#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include "sim_clock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int net_number(const char *text, int low, int high, int *value) {
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || !*text || *end || n < low || n > high) { errno = EINVAL; return -1; }
    *value = (int)n;
    return 0;
}
static int configure(int fd) {
    int flags = fcntl(fd, F_GETFL);
    int yes = 1;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) return -1;
    return 0;
}
void net_init(Connection *c, int fd, size_t max_body) {
    *c = (Connection){ .fd = fd, .max_body = max_body, .last_input = monotonic_ns() };
}
void net_close(Connection *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->input); free(c->output);
    net_init(c, -1, c->max_body);
}
bool net_pending(const Connection *c) { return c->output_offset < c->output_size; }
static uint32_t decode(const unsigned char *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}
static void encode(unsigned char *p, uint32_t value) {
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}
int net_read(Connection *c) {
    size_t limit = c->max_body + NET_HEADER;
    if (c->input_size == limit) { errno = EMSGSIZE; return -1; }
    size_t capacity = c->input_size + 8192;
    if (capacity > limit) capacity = limit;
    if (capacity > c->input_capacity) {
        void *buffer = realloc(c->input, capacity);
        if (!buffer) return -1;
        c->input = buffer; c->input_capacity = capacity;
    }
    ssize_t n = recv(c->fd, c->input + c->input_size, capacity - c->input_size, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
    c->input_size += (size_t)n;
    c->last_input = monotonic_ns();
    return 0;
}
int net_next(Connection *c, NetFrame *f) {
    if (c->input_size < NET_HEADER) return 0;
    uint32_t type = decode(c->input + 4), size = decode(c->input + 8);
    if (decode(c->input) != NET_MAGIC || type < NET_HELLO || type > NET_PONG || size > c->max_body) {
        errno = EPROTO; return -1;
    }
    if (c->input_size < NET_HEADER + size) return 0;
    char *body = malloc((size_t)size + 1);
    if (!body) return -1;
    memcpy(body, c->input + NET_HEADER, size); body[size] = 0;
    if (memchr(body, 0, size)) { free(body); errno = EPROTO; return -1; }
    *f = (NetFrame){ .type = type, .seq = decode(c->input + 12), .size = size, .body = body };
    c->input_size -= NET_HEADER + size;
    memmove(c->input, c->input + NET_HEADER + size, c->input_size);
    return 1;
}
void net_frame_free(NetFrame *f) { free(f->body); f->body = NULL; }
int net_queue(Connection *c, uint32_t type, uint32_t seq, const char *body) {
    size_t size = strlen(body), remaining = c->output_size - c->output_offset;
    if (size > NET_MAX_BODY || remaining > NET_MAX_OUTPUT - NET_HEADER - size) {
        errno = ENOBUFS; return -1;
    }
    if (c->output_offset && remaining) memmove(c->output, c->output + c->output_offset, remaining);
    c->output_offset = 0; c->output_size = remaining;
    void *buffer = realloc(c->output, remaining + NET_HEADER + size);
    if (!buffer) return -1;
    c->output = buffer;
    unsigned char *p = c->output + remaining;
    encode(p, NET_MAGIC); encode(p + 4, type); encode(p + 8, (uint32_t)size); encode(p + 12, seq);
    memcpy(p + NET_HEADER, body, size);
    c->output_size += NET_HEADER + size;
    if (!remaining) c->queued_at = monotonic_ns();
    return 0;
}
int net_flush(Connection *c) {
    // За один проход ограничиваем работу: другие соединения тоже должны обслуживаться.
    if (!net_pending(c)) return 0;
    size_t size = c->output_size - c->output_offset;
    if (size > 65536) size = 65536;
    ssize_t n = send(c->fd, c->output + c->output_offset, size, MSG_NOSIGNAL);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n <= 0) return -1;
    c->output_offset += (size_t)n;
    if (!net_pending(c)) { free(c->output); c->output = NULL; c->output_size = c->output_offset = 0; }
    return 0;
}
static int addresses(const char *host, const char *port, bool passive, struct addrinfo **result) {
    int number;
    if (net_number(port, 1, 65535, &number) < 0) return -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                             .ai_flags = passive ? AI_PASSIVE : 0 };
    if (getaddrinfo(host, port, &hints, result)) { errno = EINVAL; return -1; }
    return 0;
}
int net_listen(const char *host, const char *port) {
    struct addrinfo *list;
    if (addresses(host, port, true, &list) < 0) return -1;
    int fd = -1, saved = EADDRNOTAVAIL;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { saved = errno; continue; }
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0 && configure(fd) == 0 &&
            bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 128) == 0) break;
        saved = errno; close(fd); fd = -1;
    }
    freeaddrinfo(list); if (fd < 0) errno = saved;
    return fd;
}
int net_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo *list;
    if (addresses(host, port, false, &list) < 0) return -1;
    int fd = -1, saved = ECONNREFUSED;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { saved = errno; continue; }
        if (configure(fd) == 0) {
            int rc = connect(fd, a->ai_addr, a->ai_addrlen);
            if (rc == 0) break;
            if (errno == EINPROGRESS) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                rc = poll(&p, 1, timeout_ms);
                if (rc > 0) {
                    int error = 0; socklen_t size = sizeof(error);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error) break;
                    errno = error ? error : ECONNREFUSED;
                } else if (!rc) errno = ETIMEDOUT;
            }
        }
        saved = errno; close(fd); fd = -1;
    }
    freeaddrinfo(list); if (fd < 0) errno = saved;
    return fd;
}
int net_accept(int listener) {
    int fd = accept(listener, NULL, NULL);
    if (fd >= 0 && configure(fd) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}
