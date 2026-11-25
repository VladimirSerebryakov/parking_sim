#include "model.h"
#include "dispatcher.h"
#include "traffic.h"
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const type_names[3] = {"Обычный", "Крупногабаритный", "Специальный"};
// Интерфейс прежних предметных модулей; журнал пишет только сервер.
void record_event(Parking *p, const char *format, ...) {
    char event[EVENT_LEN], line[EVENT_LEN + 64];
    va_list args;
    va_start(args, format); vsnprintf(event, sizeof(event), format, args); va_end(args);
    int n = snprintf(line, sizeof(line), "[Такт %lld] %s\n", now_ticks(&p->state->clock), event);
    if (p->log_fd >= 0) write_all(p->log_fd, line, (size_t)n);
    write_all(STDOUT_FILENO, line, (size_t)n);
    for (int i = HISTORY_SIZE - 1; i > 0; i--) memcpy(p->state->history[i], p->state->history[i - 1], EVENT_LEN);
    memcpy(p->state->history[0], event, strlen(event) + 1);
}
int find_free_spot(const Parking *p, CarType type) {
    for (int i = 0; i < p->state->total_spots; i++)
        if (p->spots[i].type == type && p->spots[i].car_idx == -1) return i;
    return -1;
}
int model_create(Model *m, const SimParams *params) {
    *m = (Model){ .parking = { .log_fd = -1 }, .rng = params->seed + 1U };
    Parking *p = &m->parking;
    p->state = calloc(1, sizeof(*p->state));
    if (!p->state) return -1;
    ParkingState *s = p->state;
    s->params = *params; s->clock.tick_ms = params->tick_ms;
    for (int i = 0; i < 3; i++) { s->total_cars += params->cars_count[i]; s->total_spots += params->spots_count[i]; }
    if (s->total_cars > 4096 || s->total_spots > 2048 || params->queue_capacity > 4096) { errno = E2BIG; return -1; }
    p->cars = calloc((size_t)s->total_cars + 1, sizeof(*p->cars));
    m->slots = calloc((size_t)s->total_cars + 1, sizeof(*m->slots));
    p->spots = calloc((size_t)s->total_spots + 1, sizeof(*p->spots));
    p->inbox_ids = calloc((size_t)s->total_cars + 1, sizeof(int));
    p->queue_ids = calloc((size_t)params->queue_capacity + 1, sizeof(int));
    if (!p->cars || !m->slots || !p->spots || !p->inbox_ids || !p->queue_ids) return -1;
    s->inbox.capacity = s->total_cars; s->queue.capacity = params->queue_capacity;
    int index = 0;
    for (int t = 0; t < 3; t++) for (int i = 0; i < params->spots_count[t]; i++, index++)
        p->spots[index] = (ParkingSpot){ .id = index + 1, .type = (CarType)t, .car_idx = -1 };
    Traffic traffic; traffic_init(&traffic, params);
    for (int i = 0; i < s->total_cars; i++) traffic_next(&traffic, params, i + 1, &p->cars[i], &m->slots[i].interval);
    p->log_fd = open(params->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (p->log_fd < 0) return -1;
    record_event(p, "--- Старт симуляции (seed: %u, стратегия: %s) ---", params->seed, strategy_name(params->strategy));
    record_event(p, "Параметры: места %d/%d/%d; очередь %d", params->spots_count[0], params->spots_count[1], params->spots_count[2], params->queue_capacity);
    return 0;
}
void model_destroy(Model *m) {
    Parking *p = &m->parking;
    if (p->log_fd >= 0) close(p->log_fd);
    free(p->state); free(p->cars); free(p->spots); free(p->inbox_ids); free(p->queue_ids); free(m->slots);
}
void model_start(Model *m) {
    if (!m->parking.state->clock.start_ns) m->parking.state->clock.start_ns = monotonic_ns();
}
static void finish(Model *m, int i) {
    if (!m->slots[i].finished) { m->slots[i].finished = true; m->parking.state->cars_finished++; }
}
static void changed(void *context, int i) {
    Model *m = context;
    Parking *p = &m->parking;
    Car *car = &p->cars[i];
    if (car->status == CAR_PARKED) {
        // Назначение сразу начинает стоянку: обрыв не оставит вечную бронь.
        m->slots[i].park_end = monotonic_ns() + ticks_ns(&p->state->clock, car->parking_time);
        record_event(p, "Начало стоянки: авто #%d на месте #%d (%d т.)", car->id, car->spot_idx + 1, car->parking_time);
    } else if (car->status != CAR_QUEUED && car->status != CAR_ARRIVING) finish(m, i);
    m->revision++;
}
int model_arrive(Model *m, int id, int type) {
    Parking *p = &m->parking;
    if (id < 1 || id > p->state->total_cars || type != (int)p->cars[id - 1].type) { errno = EINVAL; return -1; }
    int i = id - 1;
    if (m->slots[i].arrived) return 0;
    if (p->state->stop) { errno = ECANCELED; return -1; }
    model_start(m);
    Car *car = &p->cars[i];
    m->slots[i].arrived = true;
    m->slots[i].deadline = monotonic_ns() + ticks_ns(&p->state->clock, p->state->params.max_wait_time);
    car->arrival_tick = now_ticks(&p->state->clock); p->state->cars_generated++;
    record_event(p, "Генерация: авто #%d (%s), интервал %d т., стоянка %d т.", id, type_names[type], m->slots[i].interval, car->parking_time);
    record_event(p, "Прибытие: авто #%d (%s) сообщает диспетчеру номер и тип", id, type_names[type]);
    enqueue(&p->state->inbox, p->inbox_ids, i); m->revision++;
    return 0;
}
static void leave(Model *m, int i, bool stopping) {
    Parking *p = &m->parking;
    Car *car = &p->cars[i];
    ParkingSpot *spot = &p->spots[car->spot_idx];
    record_event(p, stopping ? "Остановка: авто #%d досрочно покидает место #%d" :
                 "Окончание стоянки: авто #%d покидает место #%d", car->id, spot->id);
    car->status = CAR_DONE; spot->car_idx = -1;
    record_event(p, "Освобождение места #%d (%s)", spot->id, type_names[spot->type]);
    finish(m, i); m->revision++;
}
void model_advance(Model *m) {
    Parking *p = &m->parking;
    long long now = monotonic_ns();
    for (int i = 0; i < p->state->total_cars; i++)
        if (m->slots[i].arrived && p->cars[i].status == CAR_PARKED && now >= m->slots[i].park_end) leave(m, i, false);
    for (int q = 0; q < p->state->queue.size;) {
        int i = queue_get(&p->state->queue, p->queue_ids, q);
        if (now < m->slots[i].deadline) { q++; continue; }
        queue_remove_at(&p->state->queue, p->queue_ids, q);
        p->cars[i].status = CAR_TIMEOUT; p->state->cars_left_queue++;
        record_event(p, "Уход из очереди: авто #%d не дождалось места за %lld т.", i + 1, now_ticks(&p->state->clock) - p->cars[i].arrival_tick);
        changed(m, i);
    }
    dispatch_cars(p, &m->rng, changed, m);
}
void model_stop(Model *m) {
    Parking *p = &m->parking;
    p->state->stop = true;
    cancel_waiting(p, changed, m);
    for (int i = 0; i < p->state->total_cars; i++) if (m->slots[i].arrived && p->cars[i].status == CAR_PARKED) leave(m, i, true);
    m->revision++;
}
bool model_done(const Model *m) { return m->parking.state->cars_finished == m->parking.state->total_cars; }
void model_state(const Model *m, int id, char *text, size_t size) {
    const Car *car = &m->parking.cars[id - 1];
    long long remaining = car->status == CAR_PARKED ? m->slots[id - 1].park_end - monotonic_ns() : 0;
    if (remaining < 0) remaining = 0;
    snprintf(text, size, "%d %d %d %d %d %lld %lld", id, car->type, car->status, car->spot_idx + 1,
             car->parking_time, (remaining + 999999) / 1000000, now_ticks(&m->parking.state->clock));
}
void model_summary(const Model *m, const char *reason, char *text, size_t size) {
    const ParkingState *s = m->parking.state;
    snprintf(text, size, "\n %s\n ИТОГОВАЯ СТАТИСТИКА:\n - Обслужено автомобилей: %d\n"
             " - Ушло по таймауту из очереди: %d\n - Отказано (очередь заполнена): %d\n"
             " - Ожидание прервано остановкой: %d\n - Стратегия: %s, seed: %u\n",
             reason, s->cars_served, s->cars_left_queue, s->cars_rejected, s->cars_cancelled,
             strategy_name(s->params.strategy), s->params.seed);
}
