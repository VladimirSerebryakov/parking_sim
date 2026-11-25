// Проверка реального net.c без сокетов: разбор потока и частичная отправка.
#include "net.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static unsigned char wire[NET_MAX_BODY + NET_HEADER];
static size_t wire_size, sent;
static int send_mode, receive_mode;
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_recv(int fd, void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    (void)fd;
    assert(flags & MSG_NOSIGNAL);
    if (send_mode) { errno = send_mode; send_mode = 0; return -1; }
    if (size > 3) size = 3;
    assert(sent + size <= sizeof(wire)); memcpy(wire + sent, data, size); sent += size;
    return (ssize_t)size;
}
ssize_t __wrap_recv(int fd, void *data, size_t size, int flags) {
    (void)fd; (void)flags;
    if (receive_mode) { errno = receive_mode; receive_mode = 0; return -1; }
    if (!wire_size) return 0;
    if (size > 1) size = 1;
    memcpy(data, wire, size); memmove(wire, wire + size, wire_size - size); wire_size -= size;
    return (ssize_t)size;
}
static void feed(Connection *c, const void *data, size_t size) {
    void *next = realloc(c->input, c->input_size + size);
    assert(next); c->input = next; c->input_capacity = c->input_size + size;
    memcpy(c->input + c->input_size, data, size); c->input_size += size;
}
static void header(unsigned char *out, unsigned magic, unsigned type, unsigned size, unsigned seq) {
    uint32_t v[] = { htonl(magic), htonl(type), htonl(size), htonl(seq) }; memcpy(out, v, sizeof(v));
}
int main(void) {
    Connection sender, receiver; NetFrame f;
    net_init(&sender, -1, NET_MAX_BODY); net_init(&receiver, -1, NET_MAX_BODY);
    assert(net_queue(&sender, NET_HELLO, 1, "1 7") == 0);
    assert(net_queue(&sender, NET_PING, 2, "") == 0);
    send_mode = EINTR; assert(net_flush(&sender) == 0 && sent == 0);
    send_mode = EAGAIN; assert(net_flush(&sender) == 0 && sent == 0);
    while (net_pending(&sender)) assert(net_flush(&sender) == 0);
    assert(sent == 35);
    for (size_t i = 0; i < 19; i++) {
        feed(&receiver, wire + i, 1);
        int rc = net_next(&receiver, &f);
        if (i < 18) assert(rc == 0);
        else { assert(rc == 1 && f.type == NET_HELLO && f.seq == 1 && strcmp(f.body, "1 7") == 0); net_frame_free(&f); }
    }
    feed(&receiver, wire + 19, 16);
    assert(net_next(&receiver, &f) == 1 && f.type == NET_PING && f.seq == 2); net_frame_free(&f);
    assert(net_next(&receiver, &f) == 0);
    // Несколько полных сообщений в одном чтении сохраняют порядок.
    feed(&receiver, wire, 35);
    assert(net_next(&receiver, &f) == 1 && f.seq == 1); net_frame_free(&f);
    assert(net_next(&receiver, &f) == 1 && f.seq == 2); net_frame_free(&f);
    unsigned char bad[17];
    header(bad, NET_MAGIC + 1, NET_HELLO, 0, 1); feed(&receiver, bad, 16);
    assert(net_next(&receiver, &f) == -1 && errno == EPROTO); receiver.input_size = 0;
    header(bad, NET_MAGIC, 999, 0, 1); feed(&receiver, bad, 16);
    assert(net_next(&receiver, &f) == -1); receiver.input_size = 0;
    header(bad, NET_MAGIC, NET_HELLO, NET_MAX_BODY + 1, 1); feed(&receiver, bad, 16);
    assert(net_next(&receiver, &f) == -1); receiver.input_size = 0;
    header(bad, NET_MAGIC, NET_HELLO, 1, 1); bad[16] = 0; feed(&receiver, bad, 17);
    assert(net_next(&receiver, &f) == -1); receiver.input_size = 0;
    // Фактический net_read: EINTR/EAGAIN, фрагмент длиной один байт и EOF.
    wire_size = 35;
    receive_mode = EINTR; assert(net_read(&receiver) == 0 && !receiver.input_size);
    receive_mode = EAGAIN; assert(net_read(&receiver) == 0 && !receiver.input_size);
    for (int i = 0; i < 35; i++) assert(net_read(&receiver) == 0);
    assert(net_next(&receiver, &f) == 1 && f.seq == 1); net_frame_free(&f);
    assert(net_next(&receiver, &f) == 1 && f.seq == 2); net_frame_free(&f);
    assert(net_read(&receiver) == -1 && errno == ECONNRESET);
    // Очередь отправки имеет жёсткий предел.
    char *large = malloc(NET_MAX_BODY + 1); assert(large);
    memset(large, 'x', NET_MAX_BODY); large[NET_MAX_BODY] = 0;
    assert(net_queue(&sender, NET_SNAPSHOT, 0, large) == 0);
    assert(net_queue(&sender, NET_SNAPSHOT, 0, large) == -1 && errno == ENOBUFS);
    free(large); net_close(&sender); net_close(&receiver);
    return 0;
}
