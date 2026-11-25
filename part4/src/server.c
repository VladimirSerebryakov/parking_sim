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

static int serve(Server *s) {
    struct pollfd descriptors[MAX_PEERS + 1];
    for (;;) {
        if (!s->stopping_at) {
            model_advance(&s->model);
            ParkingState *state = s->model.parking.state;
            if (tcp_interrupted) server_stop(s, tcp_interrupted == SIGINT ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" : "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)");
            else if (model_done(&s->model)) server_stop(s, "СИМУЛЯЦИЯ ЗАВЕРШЕНА");
            else if (state->clock.start_ns && state->params.max_ticks &&
                     monotonic_ns() - state->clock.start_ns >= ticks_ns(&state->clock, state->params.max_ticks))
                server_stop(s, "СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)");
        }
        int peers = 0;
        descriptors[0] = (struct pollfd){ .fd = s->listener, .events = POLLIN };
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &s->peers[i];
            if (p->connection.fd >= 0) {
                long long now = monotonic_ns();
                if ((!s->stopping_at && now - p->connection.last_input > NET_TIMEOUT_NS) ||
                    (net_pending(&p->connection) && now - p->connection.queued_at > NET_TIMEOUT_NS)) server_drop(p);
                else if (!p->closing && !s->stopping_at) {
                    if (p->role == ROLE_CAR && p->last_status != (int)s->model.parking.cars[p->id - 1].status) server_state(s, p, 0);
                    else if (p->role == ROLE_MONITOR && now >= p->snapshot_at && !net_pending(&p->connection)) server_snapshot(s, p);
                }
                if (p->closing && !net_pending(&p->connection)) server_drop(p);
                if (p->connection.fd >= 0) peers++;
            }
            descriptors[i + 1] = (struct pollfd){ .fd = p->connection.fd,
                .events = (p->closing ? 0 : POLLIN) | (net_pending(&p->connection) ? POLLOUT : 0) };
        }
        if (s->stopping_at && (!peers || monotonic_ns() - s->stopping_at >= 2000000000LL)) return 0;
        int rc = poll(descriptors, MAX_PEERS + 1, 5);
        if (rc < 0 && errno != EINTR) return -1;
        if (rc < 0) continue;
        if (descriptors[0].revents & POLLIN) for (int accepted = 0; accepted < 32; accepted++) {
            int fd = net_accept(s->listener);
            if (fd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break; return -1; }
            int i;
            for (i = 0; i < MAX_PEERS; i++) if (s->peers[i].connection.fd < 0) break;
            if (i == MAX_PEERS) close(fd);
            else { net_init(&s->peers[i].connection, fd, 512); s->peers[i].last_status = -1; }
        }
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &s->peers[i];
            if (p->connection.fd < 0 || descriptors[i + 1].fd != p->connection.fd) continue;
            short events = descriptors[i + 1].revents;
            if (events & POLLIN) {
                if (net_read(&p->connection) < 0) { server_drop(p); continue; }
            } else if (events & (POLLERR | POLLHUP | POLLNVAL)) { server_drop(p); continue; }
            for (int count = 0; !p->closing && p->connection.fd >= 0 && count < 32; count++) {
                NetFrame f;
                int next = net_next(&p->connection, &f);
                if (next < 0) { server_drop(p); break; }
                if (!next) break;
                server_request(s, p, &f); net_frame_free(&f);
            }
            if (p->connection.fd >= 0 && net_flush(&p->connection) < 0) server_drop(p);
        }
    }
}
int main(int argc, char **argv) {
    if (tcp_signals() < 0) { report_error("sigaction"); return 1; }
    if (argc < 3) { client_print("Использование: parking_server IP ПОРТ [флаги модели]\n"); return 1; }
    SimParams params;
    if (parse_params(argc - 2, argv + 2, &params) < 0) return 1;
    Server s = { .listener = -1, .session = monotonic_ns() };
    s.peers = calloc(MAX_PEERS, sizeof(*s.peers));
    if (!s.peers) { report_error("таблица соединений"); return 1; }
    for (int i = 0; i < MAX_PEERS; i++) s.peers[i].connection.fd = -1;
    s.listener = net_listen(argv[1], argv[2]);
    if (s.listener < 0) { report_error("TCP listen"); free(s.peers); return 1; }
    int rc = model_create(&s.model, &params);
    if (rc < 0) report_error("модель парковки или журнал");
    else {
        char ready[256]; snprintf(ready, sizeof(ready), "Сервер готов: %s:%s, сеанс %lld\n", argv[1], argv[2], s.session);
        client_print(ready);
        rc = serve(&s);
        if (rc < 0) report_error("цикл сервера");
    }
    if (s.listener >= 0) close(s.listener);
    for (int i = 0; i < MAX_PEERS; i++) server_drop(&s.peers[i]);
    free(s.peers); model_destroy(&s.model);
    return rc == 0 ? 0 : 1;
}
