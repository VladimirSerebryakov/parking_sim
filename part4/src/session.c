#include "server.h"
#include "client.h"
#include "model.h"
#include "io.h"
#include "ui.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void server_drop(Peer *p) {
    net_close(&p->connection);
    *p = (Peer){ .connection = { .fd = -1 }, .last_status = -1 };
}
static int active(const Server *s, int role, int id) {
    for (int i = 0; i < MAX_PEERS; i++) {
        const Peer *p = &s->peers[i];
        if (p->connection.fd >= 0 && !p->closing && p->role == role && p->id == id) return 1;
    }
    return 0;
}
static void send_text(Peer *p, uint32_t type, uint32_t seq, const char *text) {
    if (net_queue(&p->connection, type, seq, text) < 0) server_drop(p);
}
static void reject(Peer *p, uint32_t seq, const char *text) {
    send_text(p, NET_ERROR, seq, text); p->closing = true;
}
void server_state(Server *s, Peer *p, uint32_t seq) {
    char state[256]; model_state(&s->model, p->id, state, sizeof(state));
    p->last_status = s->model.parking.cars[p->id - 1].status;
    send_text(p, NET_STATE, seq, state);
}
void server_snapshot(Server *s, Peer *p) {
    int length = 0;
    char *screen = build_dashboard(&s->model.parking, &length);
    if (!screen || length < 0 || (unsigned)length > NET_MAX_BODY) {
        free(screen); server_drop(p); return;
    }
    send_text(p, NET_SNAPSHOT, 0, screen); free(screen);
    int ms = s->model.parking.state->params.tick_ms;
    if (ms < 50) ms = 50;
    if (ms > 500) ms = 500;
    p->snapshot_at = monotonic_ns() + (long long)ms * 1000000LL;
}
void server_request(Server *s, Peer *p, const NetFrame *f) {
    if (!f->seq || f->seq <= p->seq) { reject(p, f->seq, "Неверная последовательность команд"); return; }
    p->seq = f->seq;
    if (f->type == NET_PING && !f->size) { send_text(p, NET_PONG, f->seq, ""); return; }
    if (f->type == NET_ACK && p->stop_sent && !f->size) { p->closing = true; return; }
    if (f->type == NET_DETACH && p->role && !f->size) {
        send_text(p, NET_ACK, f->seq, ""); p->closing = true; return;
    }
    if (s->stopping_at) {
        // При коротком лимите дочерний клиент может подключиться уже после STOP генератору.
        if (!p->stop_sent) { send_text(p, NET_STOP, 0, s->summary); p->stop_sent = true; }
        return;
    }
    ParkingState *state = s->model.parking.state;
    if (f->type == NET_HELLO && !p->role) {
        long long v[2];
        if (text_numbers(f->body, v, 2) < 0 || v[0] < ROLE_CAR || v[0] > ROLE_MONITOR ||
            (v[0] == ROLE_CAR ? v[1] < 1 || v[1] > state->total_cars : v[1] != 0)) {
            reject(p, f->seq, "Неверная роль или номер автомобиля"); return;
        }
        if (v[0] != ROLE_MONITOR && active(s, (int)v[0], (int)v[1])) {
            reject(p, f->seq, "Этот участник уже подключён"); return;
        }
        p->role = (int)v[0]; p->id = (int)v[1];
        char welcome[256];
        int type = p->role == ROLE_CAR ? s->model.parking.cars[p->id - 1].type : 0;
        snprintf(welcome, sizeof(welcome), "%d %d %d %d %u %lld", p->id, type,
                 state->params.tick_ms, state->total_cars, state->params.seed, s->session);
        send_text(p, NET_WELCOME, f->seq, welcome);
        if (p->connection.fd < 0) return;
        if (p->role == ROLE_CAR) server_state(s, p, 0);
        else if (p->role == ROLE_MONITOR) server_snapshot(s, p);
        else model_start(&s->model);
        return;
    }
    if (!p->role) { reject(p, f->seq, "Сначала требуется HELLO"); return; }
    if (f->type == NET_ARRIVE && p->role == ROLE_CAR) {
        long long v[2];
        if (text_numbers(f->body, v, 2) < 0 || v[0] != p->id || v[1] < 0 || v[1] > 2 ||
            model_arrive(&s->model, p->id, (int)v[1]) < 0) {
            reject(p, f->seq, "Неверные номер или тип при прибытии"); return;
        }
        model_advance(&s->model); server_state(s, p, f->seq); return;
    }
    if (f->type == NET_LEAVE && p->role == ROLE_CAR && !f->size) {
        // Срок сервера авторитетен: ранняя или повторная команда не меняет статистику.
        model_advance(&s->model); server_state(s, p, f->seq); return;
    }
    if (f->type == NET_PLAN && p->role == ROLE_GENERATOR) {
        int id;
        if (net_number(f->body, 1, state->total_cars, &id) < 0) { reject(p, f->seq, "Неверный номер плана"); return; }
        ModelCar *slot = &s->model.slots[id - 1];
        char plan[128];
        snprintf(plan, sizeof(plan), "%d %d %d %d %d", id, slot->interval, slot->arrived, slot->finished, active(s, ROLE_CAR, id));
        send_text(p, NET_PLAN_REPLY, f->seq, plan); return;
    }
    reject(p, f->seq, "Недопустимая команда для этой роли");
}
void server_stop(Server *s, const char *reason) {
    if (s->stopping_at) return;
    s->stopping_at = monotonic_ns();
    // Регистрация модели закрыта, но TCP-подключения ещё принимаются для доставки STOP.
    record_event(&s->model.parking, "%s", reason);
    model_stop(&s->model);
    model_summary(&s->model, reason, s->summary, sizeof(s->summary));
    client_print(s->summary);
    write_all(s->model.parking.log_fd, s->summary, strlen(s->summary));
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &s->peers[i];
        if (p->connection.fd < 0 || p->closing) continue;
        if (p->role == ROLE_CAR) server_state(s, p, 0);
        if (p->role == ROLE_MONITOR) server_snapshot(s, p);
        if (p->connection.fd < 0) continue;
        send_text(p, NET_STOP, 0, s->summary); p->stop_sent = true;
    }
}
