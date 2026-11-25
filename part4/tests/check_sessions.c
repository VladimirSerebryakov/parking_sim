// Реальные обработчики протокола проверяются в памяти, без TCP-соединений.
#include "server.h"
#include "io.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

static void open_peer(Peer *p) {
    *p = (Peer){ .last_status = -1 };
    int fd = open("/dev/null", O_RDWR); assert(fd >= 0);
    net_init(&p->connection, fd, 512);
}
static void command(Server *s, Peer *p, unsigned type, unsigned seq, char *body) {
    NetFrame f = { .type = type, .seq = seq, .body = body, .size = strlen(body) };
    server_request(s, p, &f);
}
static int take(Peer *p, char *text, size_t size) {
    Connection local;
    net_init(&local, -1, NET_MAX_BODY);
    // Получателем выступает реальный декодер. Системные send/recv не вызываются.
    local.input_size = p->connection.output_size;
    local.input = malloc(local.input_size); assert(local.input);
    memcpy(local.input, p->connection.output, local.input_size);
    NetFrame f; assert(net_next(&local, &f) == 1);
    int type = (int)f.type;
    if (text) { assert(f.size < size); memcpy(text, f.body, f.size + 1); }
    size_t used = p->connection.output_size - local.input_size;
    memmove(p->connection.output, p->connection.output + used, local.input_size);
    p->connection.output_size = local.input_size;
    net_frame_free(&f); net_close(&local);
    return type;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    SimParams params = { .spots_count = {1, 0, 0}, .cars_count = {4, 0, 0}, .queue_capacity = 3,
        .tick_ms = 10, .park_time_min = 1000, .park_time_max = 1000, .max_wait_time = 1000, .seed = 7 };
    assert(strlen(argv[1]) < sizeof(params.log_path)); strcpy(params.log_path, argv[1]);
    Server s = { .listener = -1, .session = 123 };
    s.peers = calloc(MAX_PEERS, sizeof(*s.peers)); assert(s.peers);
    for (int i = 0; i < MAX_PEERS; i++) s.peers[i].connection.fd = -1;
    assert(model_create(&s.model, &params) == 0);
    Peer *monitor = &s.peers[0], *car = &s.peers[1], *other = &s.peers[2], *generator = &s.peers[3];
    open_peer(monitor); command(&s, monitor, NET_HELLO, 1, "3 0");
    assert(take(monitor, NULL, 0) == NET_WELCOME && take(monitor, NULL, 0) == NET_SNAPSHOT);
    assert(!s.model.parking.state->clock.start_ns);
    open_peer(car); command(&s, car, NET_HELLO, 1, "1 1");
    assert(take(car, NULL, 0) == NET_WELCOME && take(car, NULL, 0) == NET_STATE);
    open_peer(other); command(&s, other, NET_HELLO, 1, "1 1");
    assert(take(other, NULL, 0) == NET_ERROR && other->closing); server_drop(other);
    command(&s, car, NET_ARRIVE, 2, "1 0"); assert(take(car, NULL, 0) == NET_STATE);
    command(&s, car, NET_ARRIVE, 3, "1 0"); assert(take(car, NULL, 0) == NET_STATE);
    command(&s, car, NET_LEAVE, 4, ""); assert(take(car, NULL, 0) == NET_STATE);
    assert(s.model.parking.state->cars_generated == 1 && s.model.parking.state->cars_served == 1);
    assert(s.model.parking.cars[0].status == CAR_PARKED);
    command(&s, car, NET_DETACH, 5, ""); assert(take(car, NULL, 0) == NET_ACK); server_drop(car);
    open_peer(other); command(&s, other, NET_HELLO, 1, "1 1");
    assert(take(other, NULL, 0) == NET_WELCOME && take(other, NULL, 0) == NET_STATE);
    command(&s, other, NET_ARRIVE, 2, "1 0"); assert(take(other, NULL, 0) == NET_STATE);
    assert(s.model.parking.state->cars_generated == 1);
    open_peer(generator); command(&s, generator, NET_HELLO, 1, "2 0");
    assert(take(generator, NULL, 0) == NET_WELCOME);
    char text[256]; long long v[5];
    command(&s, generator, NET_PLAN, 2, "1");
    assert(take(generator, text, sizeof(text)) == NET_PLAN_REPLY);
    assert(text_numbers(text, v, 5) == 0 && v[2] == 1 && v[3] == 0 && v[4] == 1);
    open_peer(car); command(&s, car, NET_HELLO, 1, "2 0");
    assert(take(car, NULL, 0) == NET_ERROR); server_drop(car);
    server_drop(generator); open_peer(generator); command(&s, generator, NET_HELLO, 1, "2 0");
    assert(take(generator, NULL, 0) == NET_WELCOME);
    // Терминальный результат сохраняется после разрыва и не изменяется повтором ARRIVE.
    s.model.slots[0].park_end = 1; model_advance(&s.model);
    server_drop(other); open_peer(other); command(&s, other, NET_HELLO, 1, "1 1");
    assert(take(other, NULL, 0) == NET_WELCOME && take(other, text, sizeof(text)) == NET_STATE);
    long long state[7]; assert(text_numbers(text, state, 7) == 0 && state[2] == CAR_DONE);
    command(&s, other, NET_ARRIVE, 2, "1 0"); assert(take(other, NULL, 0) == NET_STATE);
    assert(s.model.parking.state->cars_served == 1 && s.model.parking.state->cars_finished == 1);
    Peer *second = &s.peers[4], *third = &s.peers[5];
    open_peer(second); command(&s, second, NET_HELLO, 1, "1 2");
    assert(take(second, NULL, 0) == NET_WELCOME && take(second, NULL, 0) == NET_STATE);
    command(&s, second, NET_ARRIVE, 2, "2 0"); assert(take(second, NULL, 0) == NET_STATE);
    open_peer(third); command(&s, third, NET_HELLO, 1, "1 3");
    assert(take(third, NULL, 0) == NET_WELCOME && take(third, NULL, 0) == NET_STATE);
    command(&s, third, NET_ARRIVE, 2, "3 0"); assert(take(third, NULL, 0) == NET_STATE);
    assert(s.model.parking.cars[2].status == CAR_QUEUED);
    long long deadline = s.model.slots[2].deadline;
    command(&s, third, NET_DETACH, 3, ""); assert(take(third, NULL, 0) == NET_ACK); server_drop(third);
    open_peer(third); command(&s, third, NET_HELLO, 1, "1 3");
    assert(take(third, NULL, 0) == NET_WELCOME && take(third, text, sizeof(text)) == NET_STATE);
    assert(text_numbers(text, state, 7) == 0 && state[2] == CAR_QUEUED);
    command(&s, third, NET_ARRIVE, 2, "3 0"); assert(take(third, NULL, 0) == NET_STATE);
    assert(s.model.slots[2].deadline == deadline && s.model.parking.state->queue.size == 1);
    // Наблюдатель не вправе посылать команды автомобиля.
    Peer *invalid = &s.peers[6]; open_peer(invalid); command(&s, invalid, NET_HELLO, 1, "3 0");
    assert(take(invalid, NULL, 0) == NET_WELCOME && take(invalid, NULL, 0) == NET_SNAPSHOT);
    command(&s, invalid, NET_ARRIVE, 2, "4 0"); assert(take(invalid, NULL, 0) == NET_ERROR); server_drop(invalid);
    Peer *connecting = &s.peers[7]; open_peer(connecting);
    server_stop(&s, "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)");
    assert(take(connecting, NULL, 0) == NET_STOP);
    command(&s, connecting, NET_ACK, 1, ""); assert(connecting->closing);
    Peer *late = &s.peers[8]; open_peer(late);
    command(&s, late, NET_HELLO, 1, "1 4"); assert(take(late, NULL, 0) == NET_STOP);
    command(&s, late, NET_ACK, 2, ""); assert(late->closing);
    assert(take(monitor, NULL, 0) == NET_SNAPSHOT && take(monitor, NULL, 0) == NET_STOP);
    assert(take(generator, NULL, 0) == NET_STOP);
    assert(take(other, NULL, 0) == NET_STATE && take(other, NULL, 0) == NET_STOP);
    command(&s, monitor, NET_ACK, 2, ""); command(&s, other, NET_ACK, 3, ""); command(&s, generator, NET_ACK, 2, "");
    assert(monitor->closing && other->closing && generator->closing);
    for (int i = 0; i < MAX_PEERS; i++) server_drop(&s.peers[i]);
    free(s.peers); model_destroy(&s.model);
    return 0;
}
