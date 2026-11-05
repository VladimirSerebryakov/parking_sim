#include "protocol.h"
#include "dispatcher.h"
#include "traffic.h"
#include "io.h"
#include "ui.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    pid_t pid; // Программа участника
    uint64_t cookie; // Подключение, независимо от повторного использования PID
    bool seen, active, arrived, finished, started; // Этапы протокола
    int interval; // План прибытия машины
    long long deadline; // Срок ожидания, CLOCK_MONOTONIC
} Peer;
typedef struct Outgoing {
    Message message; // Ответ до появления места в очереди
    struct Outgoing *next; // Локальная очередь диспетчера
} Outgoing;
typedef struct {
    Transport transport; // Только очереди и именованные семафоры
    Parking parking; // Полностью локальная модель
    Peer *peers; // 0 — генератор, остальные — автомобили
    int active, stop_signal; // Подключения и причина остановки
    bool failed, started; // Ошибка и начало моделирования
    Outgoing *head, *tail; // Исходящие ответы при переполнении MQ
} Server;

static void enqueue_reply(Server *s, Message message) {
    Outgoing *node = malloc(sizeof(*node));
    if (!node) { s->failed = true; return; }
    message.magic = MQ_MAGIC;
    node->message = message; node->next = NULL;
    if (s->tail) s->tail->next = node; else s->head = node;
    s->tail = node;
}
static void reply(Server *s, const Message *request, MessageType type, int error) {
    Message m = { .pid = request->pid, .cookie = request->cookie, .seq = request->seq,
                  .id = request->id, .type = type, .error = error, .owner_pid = getpid() };
    if (type == MQ_WELCOME) {
        m.params = s->parking.state->params;
        m.clock = s->parking.state->clock;
        if (m.id > 0) m.car = s->parking.cars[m.id - 1];
    }
    enqueue_reply(s, m);
}
static void notify_car(void *context, int index) {
    Server *s = context;
    Peer *peer = &s->peers[index + 1];
    Car *car = &s->parking.cars[index];
    if ((car->status == CAR_REJECTED || car->status == CAR_TIMEOUT || car->status == CAR_CANCELLED) &&
        !peer->finished) { peer->finished = true; s->parking.state->cars_finished++; }
    if (!peer->active) return;
    enqueue_reply(s, (Message){ .pid = peer->pid, .cookie = peer->cookie,
        .id = index + 1, .type = MQ_STATUS, .car = *car });
}
static bool same_peer(const Message *m, pid_t pid, uint64_t cookie) {
    return m->pid == pid && m->cookie == cookie;
}
// Завершившемуся клиенту больше не нужны старые уведомления; они не должны занимать MQ.
static int purge_replies(Server *s, pid_t pid, uint64_t cookie) {
    Outgoing **link = &s->head;
    s->tail = NULL;
    while (*link) {
        Outgoing *node = *link;
        if (same_peer(&node->message, pid, cookie)) { *link = node->next; free(node); }
        else { s->tail = node; link = &node->next; }
    }
    int attempts = 0;
    while (transport_lock(&s->transport) < 0) {
        if (errno != ETIMEDOUT || ++attempts == 200) return -1;
    }
    struct mq_attr attr;
    int rc = mq_getattr(s->transport.replies, &attr);
    if (rc == 0) for (long i = 0; i < attr.mq_curmsgs; i++) {
        Message m;
        if (transport_receive(s->transport.replies, &m) < 0) { rc = -1; break; }
        if (!same_peer(&m, pid, cookie) && transport_send(s->transport.replies, &m) < 0) { rc = -1; break; }
    }
    if (transport_unlock(&s->transport) < 0) rc = -1;
    return rc;
}
static int flush_replies(Server *s) {
    if (!s->head) return 0;
    if (transport_lock(&s->transport) < 0) return errno == ETIMEDOUT ? 0 : -1;
    int rc = 0;
    while (s->head) {
        Outgoing *node = s->head;
        if (transport_send(s->transport.replies, &node->message) < 0) {
            if (errno != EAGAIN) rc = -1;
            break;
        }
        if (sem_post(s->transport.sems[3]) < 0) rc = -1;
        s->head = node->next;
        free(node);
    }
    if (!s->head) s->tail = NULL;
    if (transport_unlock(&s->transport) < 0) rc = -1;
    return rc;
}
static int create_model(Server *s, const SimParams *params) {
    ParkingState *state = calloc(1, sizeof(*state));
    if (!state) return -1;
    s->parking.state = state;
    state->params = *params;
    state->clock = (SimClock){ .tick_ms = params->tick_ms };
    for (int i = 0; i < 3; i++) { state->total_cars += params->cars_count[i]; state->total_spots += params->spots_count[i]; }
    size_t cars = (size_t)state->total_cars, spots = (size_t)state->total_spots;
    s->parking.cars = calloc(cars + 1, sizeof(Car));
    s->parking.spots = calloc(spots + 1, sizeof(ParkingSpot));
    s->parking.inbox_ids = calloc(cars + 1, sizeof(int));
    s->parking.queue_ids = calloc((size_t)params->queue_capacity + 1, sizeof(int));
    s->peers = calloc(cars + 1, sizeof(Peer));
    if (!s->parking.cars || !s->parking.spots || !s->parking.inbox_ids || !s->parking.queue_ids || !s->peers) return -1;
    state->inbox.capacity = state->total_cars;
    state->queue.capacity = params->queue_capacity;
    int index = 0;
    for (int type = 0; type < 3; type++) for (int i = 0; i < params->spots_count[type]; i++, index++)
        s->parking.spots[index] = (ParkingSpot){ .id = index + 1, .type = (CarType)type, .car_idx = -1 };
    Traffic traffic; traffic_init(&traffic, params);
    for (int i = 0; i < state->total_cars; i++)
        traffic_next(&traffic, params, i + 1, &s->parking.cars[i], &s->peers[i + 1].interval);
    s->parking.log_fd = open(params->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return s->parking.log_fd < 0 ? -1 : 0;
}
static void close_server(Server *s) {
    while (s->head) { Outgoing *next = s->head->next; free(s->head); s->head = next; }
    free(s->peers); free(s->parking.cars); free(s->parking.spots);
    free(s->parking.inbox_ids); free(s->parking.queue_ids); free(s->parking.state);
    if (s->parking.log_fd >= 0) close(s->parking.log_fd);
    transport_close(&s->transport);
}
static void handle_request(Server *s, const Message *m) {
    Parking *p = &s->parking;
    ParkingState *state = p->state;
    if (m->pid <= 0 || !m->cookie || !m->seq) return;
    if (m->id < 0 || m->id > state->total_cars) { reply(s, m, MQ_ERROR, EINVAL); return; }
    Peer *peer = &s->peers[m->id];
    if (m->type == MQ_HELLO) {
        if (state->stop) { reply(s, m, MQ_ERROR, ECANCELED); return; }
        if (peer->active || (m->id > 0 && peer->seen)) { reply(s, m, MQ_ERROR, EBUSY); return; }
        *peer = (Peer){ .pid = m->pid, .cookie = m->cookie, .seen = true, .active = true,
                        .interval = peer->interval };
        s->active++;
        if (!s->started) { s->started = true; state->clock.start_ns = monotonic_ns(); }
        reply(s, m, MQ_WELCOME, 0);
        return;
    }
    if (!peer->active || peer->pid != m->pid || peer->cookie != m->cookie) { reply(s, m, MQ_ERROR, EINVAL); return; }
    if (m->type == MQ_ARRIVE && m->id > 0) {
        Car *car = &p->cars[m->id - 1];
        if (peer->arrived || m->car.id != car->id || m->car.type != car->type) {
            reply(s, m, MQ_ERROR, EPROTO); return;
        }
        peer->arrived = true;
        car->arrival_tick = now_ticks(&state->clock);
        peer->deadline = monotonic_ns() + ticks_ns(&state->clock, state->params.max_wait_time);
        state->cars_generated++;
        record_event(p, "Генерация: авто #%d (%s), интервал %d т., стоянка %d т.",
                     car->id, type_names[car->type], peer->interval, car->parking_time);
        record_event(p, "Прибытие: авто #%d (%s) сообщает диспетчеру номер и тип", m->car.id, type_names[m->car.type]);
        if (state->stop) {
            car->status = CAR_CANCELLED; state->cars_cancelled++;
            record_event(p, "Диспетчер: остановка, ожидание авто #%d прервано", car->id);
            notify_car(s, m->id - 1);
        } else enqueue(&state->inbox, p->inbox_ids, m->id - 1);
    } else if (m->type == MQ_STOP_REQUEST) {
        if (m->signal != SIGINT && m->signal != SIGTERM) { reply(s, m, MQ_ERROR, EINVAL); return; }
        if (!s->stop_signal) s->stop_signal = m->signal;
    } else if (m->type == MQ_FAIL) s->failed = true;
    else if (m->type == MQ_BYE) {
        if (m->id > 0 && !peer->finished) { reply(s, m, MQ_ERROR, EBUSY); return; }
        peer->active = false; s->active--;
        if (purge_replies(s, peer->pid, peer->cookie) < 0) s->failed = true;
    } else if (m->id > 0 && (m->type == MQ_START || m->type == MQ_LEAVE)) {
        Car *car = &p->cars[m->id - 1];
        if (car->status != CAR_PARKED || (m->type == MQ_LEAVE && !peer->started)) {
            reply(s, m, MQ_ERROR, EPROTO); return;
        }
        if (m->type == MQ_START && !peer->started) {
            peer->started = true;
            car->leave_tick = now_ticks(&state->clock) + car->parking_time;
            record_event(p, "Начало стоянки: авто #%d на месте #%d (%d т.)", car->id, car->spot_idx + 1, car->parking_time);
        } else if (m->type == MQ_LEAVE) {
            ParkingSpot *spot = &p->spots[car->spot_idx];
            record_event(p, state->stop ? "Остановка: авто #%d досрочно покидает место #%d" :
                         "Окончание стоянки: авто #%d покидает место #%d", car->id, spot->id);
            spot->car_idx = -1; car->status = CAR_DONE;
            record_event(p, "Освобождение места #%d (%s)", spot->id, type_names[spot->type]);
            peer->finished = true; state->cars_finished++;
        }
    } else { reply(s, m, MQ_ERROR, EPROTO); return; }
    reply(s, m, MQ_ACK, 0);
}
static void expire_queue(Server *s) {
    Parking *p = &s->parking;
    for (int i = 0; i < p->state->queue.size;) {
        int index = queue_get(&p->state->queue, p->queue_ids, i);
        if (monotonic_ns() < s->peers[index + 1].deadline) { i++; continue; }
        queue_remove_at(&p->state->queue, p->queue_ids, i);
        Car *car = &p->cars[index];
        car->status = CAR_TIMEOUT; p->state->cars_left_queue++;
        record_event(p, "Уход из очереди: авто #%d не дождалось места за %lld т.", car->id,
                     now_ticks(&p->state->clock) - car->arrival_tick);
        notify_car(s, index);
    }
}
static int serve(Server *s, const sigset_t *signals, const char **reason) {
    Parking *p = &s->parking;
    unsigned int rng = p->state->params.seed + 1U;
    long long stopping_since = 0, frame = 0;
    for (;;) {
        int sig = model_stop_signal(signals);
        if (sig && !s->stop_signal) s->stop_signal = sig;
        for (int i = 0; i <= p->state->total_cars; i++) {
            Peer *peer = &s->peers[i];
            if (peer->active && kill(peer->pid, 0) < 0 && errno == ESRCH) {
                peer->active = false; s->active--; s->failed = true;
                if (purge_replies(s, peer->pid, peer->cookie) < 0) return -1;
            }
        }
        long long now = monotonic_ns();
        bool done = p->state->total_cars == 0 || (s->started && p->state->cars_finished == p->state->total_cars);
        bool limit = s->started && p->state->params.max_ticks > 0 &&
            now >= p->state->clock.start_ns + ticks_ns(&p->state->clock, p->state->params.max_ticks);
        if (!p->state->stop && (s->failed || s->stop_signal || done || limit)) {
            p->state->stop = true; stopping_since = now;
            if (s->failed) record_event(p, "Ошибка участника. Остановка сеанса.");
            else if (s->stop_signal) {
                *reason = s->stop_signal == SIGINT ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" : "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)";
                record_event(p, "Получен сигнал %s - остановка симуляции", s->stop_signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM");
            } else if (!done) {
                *reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)";
                record_event(p, "Достигнут лимит времени. Завершение.");
            } else record_event(p, "Все автомобили обработаны. Завершение.");
            cancel_waiting(p, notify_car, s);
            for (int i = 0; i <= p->state->total_cars; i++) {
                Peer *peer = &s->peers[i];
                if (peer->active && (i == 0 || !peer->finished))
                    enqueue_reply(s, (Message){ .pid = peer->pid, .cookie = peer->cookie, .id = i, .type = MQ_STOP });
            }
        }
        for (int i = 0; i < MQ_CAPACITY; i++) {
            Message m;
            if (transport_receive(s->transport.requests, &m) < 0) {
                if (errno == EPROTO) { record_event(p, "Протокол: некорректное сообщение отклонено"); continue; }
                if (errno != EAGAIN) return -1;
                break;
            }
            handle_request(s, &m);
        }
        if (!p->state->stop) { expire_queue(s); dispatch_cars(p, &rng, notify_car, s); }
        if (flush_replies(s) < 0) return -1;
        if (now >= frame) {
            draw_dashboard(p);
            frame = now + ticks_ns(&p->state->clock, 1);
        }
        if (p->state->stop && s->active == 0 && !s->head) return s->failed ? -1 : 0;
        if (p->state->stop && now - stopping_since > 4000000000LL) { errno = ETIMEDOUT; return -1; }
        struct timespec wake; clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_nsec += 1000000L;
        if (wake.tv_nsec >= 1000000000L) { wake.tv_sec++; wake.tv_nsec -= 1000000000L; }
        if (sem_timedwait(s->transport.sems[2], &wake) < 0 && errno != ETIMEDOUT && errno != EINTR) return -1;
    }
}
int main(int argc, char **argv) {
    sigset_t signals;
    if (model_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc < 2) { errno = EINVAL; report_error("нужно имя сеанса"); return 1; }
    SimParams params;
    if (parse_params(argc - 1, argv + 1, &params) < 0) return 1;
    Server s = { .parking = { .log_fd = -1 } };
    if (transport_create(&s.transport, argv[1]) < 0) { report_error("создание POSIX-очередей/семафоров"); return 1; }
    if (create_model(&s, &params) < 0) { report_error("локальная модель/журнал"); close_server(&s); return 1; }
    record_event(&s.parking, "--- Старт симуляции (seed: %u, стратегия: %s) ---", params.seed, strategy_name(params.strategy));
    record_event(&s.parking, "POSIX очереди: %s_requests %s_replies", s.transport.base, s.transport.base);
    record_event(&s.parking, "Параметры: места %d/%d/%d; очередь %d", params.spots_count[0], params.spots_count[1], params.spots_count[2], params.queue_capacity);
    if (transport_publish(&s.transport) < 0) { report_error("готовность сеанса"); close_server(&s); return 1; }
    ui_enter();
    const char *reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА";
    int rc = serve(&s, &signals, &reason);
    ui_leave();
    if (rc == 0) print_final_stats(&s.parking, reason);
    else { const char err[] = "Ошибка: сеанс MQ завершён из-за ошибки участника или протокола\n"; write_all(STDERR_FILENO, err, sizeof(err)-1); }
    close_server(&s);
    return rc == 0 ? 0 : 1;
}
