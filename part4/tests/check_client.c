// Тест поведения настоящего клиента на заранее закодированных входящих сообщениях.
// Сокеты не создаются: единственная отправка (ACK) попадает в локальный буфер.
#include "client.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static Connection sent;
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags);
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    (void)fd;
    assert(flags & MSG_NOSIGNAL);
    void *buffer = realloc(sent.input, sent.input_size + size); assert(buffer);
    sent.input = buffer;
    memcpy(sent.input + sent.input_size, data, size); sent.input_size += size;
    return (ssize_t)size;
}
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout) {
    (void)fds; (void)n; (void)timeout;
    assert(!"Тест не должен ждать данных вне подготовленных сообщений");
    errno = EIO; return -1;
}
static void prepare(Client *c, const char **states, size_t count, bool stop) {
    *c = (Client){ .id = 1, .type = 0, .seq = 1 };
    net_init(&c->connection, -1, NET_MAX_BODY);
    Connection encoded;
    net_init(&encoded, -1, NET_MAX_BODY);
    for (size_t i = 0; i < count; i++) assert(net_queue(&encoded, NET_STATE, 0, states[i]) == 0);
    if (stop) assert(net_queue(&encoded, NET_STOP, 0, "Остановка сервера\n") == 0);
    c->connection.input = encoded.output; c->connection.input_size = encoded.output_size;
    c->connection.input_capacity = encoded.output_size;
    encoded.output = NULL; net_close(&encoded);
    net_init(&sent, -1, NET_MAX_BODY);
}
static int pending(Client *c, char *body, size_t size) {
    // Передаём ещё не отправленную команду только декодеру, не системному вызову.
    Connection decoder;
    net_init(&decoder, -1, NET_MAX_BODY);
    decoder.input = c->connection.output; decoder.input_size = c->connection.output_size;
    NetFrame f; assert(net_next(&decoder, &f) == 1);
    int type = (int)f.type;
    assert(f.seq > 1);
    if (body) { assert(f.size < size); memcpy(body, f.body, f.size + 1); }
    memmove(c->connection.output, decoder.input, decoder.input_size);
    c->connection.output_size = decoder.input_size;
    decoder.input = NULL; net_frame_free(&f); net_close(&decoder);
    return type;
}
int main(void) {
    Client c;
    const char *done[] = { "1 0 6 1 10 0 12" };
    prepare(&c, done, 1, false); assert(car_drive(&c) == 0);
    assert(!net_pending(&c.connection)); client_close(&c); net_close(&sent);

    const char *new_car[] = { "1 0 0 0 10 0 0", "1 0 1 0 10 0 0", "1 0 4 0 10 0 8" };
    prepare(&c, new_car, 3, false); assert(car_drive(&c) == 0);
    char body[32]; assert(pending(&c, body, sizeof(body)) == NET_ARRIVE && !strcmp(body, "1 0"));
    assert(!net_pending(&c.connection)); client_close(&c); net_close(&sent);

    const char *queued[] = { "1 0 1 0 10 0 0", "1 0 4 0 10 0 8" };
    prepare(&c, queued, 2, false); assert(car_drive(&c) == 0);
    assert(!net_pending(&c.connection)); client_close(&c); net_close(&sent);

    const char *parked[] = { "1 0 2 1 10 0 10", "1 0 6 1 10 0 10" };
    prepare(&c, parked, 2, false); assert(car_drive(&c) == 0);
    assert(pending(&c, NULL, 0) == NET_LEAVE && !net_pending(&c.connection));
    client_close(&c); net_close(&sent);

    const char *stopping[] = { "1 0 2 1 10 1000 0" };
    prepare(&c, stopping, 1, true); assert(car_drive(&c) == 0 && c.stopped);
    NetFrame f; assert(net_next(&sent, &f) == 1 && f.type == NET_ACK && f.seq == 2);
    assert(!strcmp(c.summary, "Остановка сервера\n")); net_frame_free(&f);
    client_close(&c); net_close(&sent);

    prepare(&c, stopping, 1, true); c.defer_stop_ack = true;
    assert(car_drive(&c) == 0 && c.stopped && !c.stop_ack_sent && !sent.input_size);
    client_ack_stop(&c);
    assert(net_next(&sent, &f) == 1 && f.type == NET_ACK && f.seq == 2);
    net_frame_free(&f);
    client_ack_stop(&c); assert(!sent.input_size); // Повтор не создаёт второго ACK.
    client_close(&c); net_close(&sent);

    const char *invalid[] = { "2 0 6 1 10 0 12", "1 0 2 0 10 100 12", "1 0 2 1 10 -1 12" };
    for (size_t i = 0; i < 3; i++) {
        prepare(&c, invalid + i, 1, false); assert(car_drive(&c) == -1 && errno == EPROTO);
        client_close(&c); net_close(&sent);
    }
    return 0;
}
