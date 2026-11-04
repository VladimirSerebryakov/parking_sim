#include "session.h"
#include "io.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

static void finish_car(Session *s) {
    s->slots[s->car_idx].finished = true;
    s->memory.parking.state->cars_finished++;
}

static int drive_car(Session *s, const sigset_t *signals) {
    Parking *p = &s->memory.parking;
    Car *car = &p->cars[s->car_idx];
    if (session_lock(s) < 0) return -1;
    car->arrival_tick = now_ticks(&p->state->clock);
    deadline_after(&p->state->clock, p->state->params.max_wait_time, &car->deadline);
    p->state->cars_generated++;
    record_event(p, "Генерация: авто #%d (%s), интервал %d т., стоянка %d т.",
                 car->id, type_names[car->type], s->slots[s->car_idx].interval, car->parking_time);
    record_event(p, "Прибытие: авто #%d (%s) сообщает диспетчеру номер и тип", car->id, type_names[car->type]);
    if (p->state->stop) {
        car->status = CAR_CANCELLED;
        p->state->cars_cancelled++;
        record_event(p, "Остановка: авто #%d не въезжает", car->id);
        finish_car(s);
        session_unlock(s);
        return 0;
    }
    enqueue(&p->state->inbox, p->inbox_ids, s->car_idx);
    session_unlock(s);
    if (session_wake(s) < 0) return -1;

    bool queued = false;
    for (;;) {
        int sig = session_signal(signals);
        if (sig && session_request_stop(s, sig, false) < 0) return -1;
        if (!session_owner_alive(s)) { errno = EPIPE; return -1; }
        struct timespec wake;
        clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_nsec += SESSION_WAIT_NS;
        if (wake.tv_nsec >= 1000000000L) { wake.tv_sec++; wake.tv_nsec -= 1000000000L; }
        if (queued && (car->deadline.tv_sec < wake.tv_sec ||
            (car->deadline.tv_sec == wake.tv_sec && car->deadline.tv_nsec < wake.tv_nsec)))
            wake = car->deadline;
        int rc;
        do { rc = sem_timedwait(s->grant, &wake); } while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != ETIMEDOUT) return -1;
        if (session_lock(s) < 0) return -1;
        CarStatus status = car->status;
        if (status == CAR_QUEUED) {
            queued = true;
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > car->deadline.tv_sec ||
                (now.tv_sec == car->deadline.tv_sec && now.tv_nsec >= car->deadline.tv_nsec)) {
                for (int i = 0; i < p->state->queue.size; i++) {
                    if (queue_get(&p->state->queue, p->queue_ids, i) == s->car_idx) {
                        queue_remove_at(&p->state->queue, p->queue_ids, i);
                        break;
                    }
                }
                car->status = CAR_TIMEOUT;
                p->state->cars_left_queue++;
                record_event(p, "Уход из очереди: авто #%d не дождалось места за %lld т.",
                             car->id, now_ticks(&p->state->clock) - car->arrival_tick);
                finish_car(s);
                session_unlock(s);
                return 0;
            }
        } else if (status == CAR_PARKED) {
            record_event(p, "Начало стоянки: авто #%d на месте #%d (%d т.)",
                         car->id, car->spot_idx + 1, car->parking_time);
            session_unlock(s);
            break;
        } else if (status == CAR_REJECTED || status == CAR_CANCELLED) {
            finish_car(s);
            session_unlock(s);
            return 0;
        }
        session_unlock(s);
    }

    long long end = monotonic_ns() + ticks_ns(&p->state->clock, car->parking_time);
    bool full_time;
    for (;;) {
        int sig = session_signal(signals);
        if (sig && session_request_stop(s, sig, false) < 0) return -1;
        if (!session_owner_alive(s)) { errno = EPIPE; return -1; }
        if (session_lock(s) < 0) return -1;
        bool stop = p->state->stop;
        session_unlock(s);
        long long now = monotonic_ns();
        if (stop || now >= end) { full_time = !stop; break; }
        sleep_until(end < now + SESSION_WAIT_NS ? end : now + SESSION_WAIT_NS);
    }
    if (session_lock(s) < 0) return -1;
    ParkingSpot *spot = &p->spots[car->spot_idx];
    record_event(p, full_time ? "Окончание стоянки: авто #%d покидает место #%d" :
                 "Остановка: авто #%d досрочно покидает место #%d", car->id, spot->id);
    spot->car_idx = -1;
    car->status = CAR_DONE;
    record_event(p, "Освобождение места #%d (%s)", spot->id, type_names[spot->type]);
    finish_car(s);
    session_unlock(s);
    return 0;
}

int main(int argc, char **argv) {
    sigset_t signals;
    if (session_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc != 3) {
        const char usage[] = "Использование: parking_car СЕАНС НОМЕР_АВТО\n";
        write_all(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }
    char *end;
    errno = 0;
    long id = strtol(argv[2], &end, 10);
    if (errno || !*argv[2] || *end || id < 1 || id > INT_MAX) {
        errno = EINVAL; report_error("номер автомобиля"); return 1;
    }
    Session session;
    if (session_connect(&session, argv[1], (int)id - 1) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение автомобиля"); return 1;
    }
    int rc = drive_car(&session, &signals);
    if (rc < 0) report_error("работа автомобиля");
    if (session_disconnect(&session) < 0) rc = -1;
    session_close(&session);
    return rc == 0 ? 0 : 1;
}
