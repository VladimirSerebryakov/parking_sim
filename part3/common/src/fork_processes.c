#include "fork_processes.h"
#include "io.h"
#include "dispatcher.h"
#include "ui.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK_NS 10000000LL // Родитель проверяет завершение не реже чем раз в 10 мс
#define SLEEP_STEP_NS 50000000LL // Автомобили и табло проверяют stop раз в 50 мс

typedef struct {
    pid_t pid; // PID ещё не собранного дочернего процесса, 0 после waitpid
    bool car; // Автомобиль может штатно завершиться до остановки всей модели
} Child;

// Все функции изменения модели ниже вызываются с захваченным state_lock.
static void notify_if_all_done(ForkContext *h) {
    ParkingState *s = h->parking.state;
    if (!s->end_notified && s->generator_done && s->cars_finished == s->cars_generated) {
        s->end_notified = true;
        sem_post_checked(h->sim_end);
    }
}

static void car_finished(ForkContext *h) {
    h->parking.state->cars_finished++;
    notify_if_all_done(h);
}

static void notify_car(void *context, int index) {
    ForkContext *h = context;
    sem_post_checked(h->granted[index]);
}

static void dispatcher_process(ForkContext *h) {
    unsigned int rng = h->parking.state->params.seed + 1U;
    for (;;) {
        sem_wait_retry(h->dispatcher_wake);
        state_lock(h);
        if (h->parking.state->stop) {
            cancel_waiting(&h->parking, notify_car, h);
            state_unlock(h);
            return;
        }
        dispatch_cars(&h->parking, &rng, notify_car, h);
        state_unlock(h);
    }
}

// Проверка общего stop всегда под семафором; сон — только вне критической секции.
static bool process_sleep(ForkContext *h, int ticks) {
    long long end = monotonic_ns() + ticks_ns(&h->parking.state->clock, ticks);
    for (;;) {
        state_lock(h);
        bool stop = h->parking.state->stop;
        state_unlock(h);
        if (stop) return false;
        long long now = monotonic_ns();
        if (now >= end) return true;
        sleep_until(end < now + SLEEP_STEP_NS ? end : now + SLEEP_STEP_NS);
    }
}

static void car_process(ForkContext *h, int idx) {
    Parking *p = &h->parking;
    Car *car = &p->cars[idx];
    state_lock(h);
    car->arrival_tick = now_ticks(&p->state->clock);
    deadline_after(&p->state->clock, p->state->params.max_wait_time, &car->deadline);
    record_event(p, "Прибытие: авто #%d (%s) сообщает диспетчеру номер и тип",
                 car->id, type_names[car->type]);
    if (p->state->stop) {
        car->status = CAR_CANCELLED;
        p->state->cars_cancelled++;
        record_event(p, "Остановка: авто #%d не въезжает", car->id);
        car_finished(h);
        state_unlock(h);
        return;
    }
    enqueue(&p->state->inbox, p->inbox_ids, idx);
    state_unlock(h);
    sem_post_checked(h->dispatcher_wake);

    sem_wait_retry(h->granted[idx]);
    state_lock(h);
    CarStatus status = car->status;
    state_unlock(h);
    if (status == CAR_QUEUED) {
        int rc;
        do { rc = sem_timedwait(h->granted[idx], &car->deadline); }
        while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != ETIMEDOUT) { report_error("sem_timedwait"); _exit(2); }
        state_lock(h);
        if (rc < 0 && car->status == CAR_QUEUED) {
            for (int i = 0; i < p->state->queue.size; i++) {
                if (queue_get(&p->state->queue, p->queue_ids, i) == idx) {
                    queue_remove_at(&p->state->queue, p->queue_ids, i);
                    break;
                }
            }
            car->status = CAR_TIMEOUT;
            p->state->cars_left_queue++;
            record_event(p, "Уход из очереди: авто #%d не дождалось места за %lld т.",
                         car->id, now_ticks(&p->state->clock) - car->arrival_tick);
            car_finished(h);
            state_unlock(h);
            return;
        }
        if (rc < 0 && sem_trywait(h->granted[idx]) < 0) {
            report_error("ответ диспетчера после таймаута");
            _exit(2);
        }
        status = car->status;
        state_unlock(h);
    }
    if (status != CAR_PARKED) {
        state_lock(h);
        car_finished(h);
        state_unlock(h);
        return;
    }

    state_lock(h);
    ParkingSpot *spot = &p->spots[car->spot_idx];
    record_event(p, "Начало стоянки: авто #%d на месте #%d (%d т.)",
                 car->id, spot->id, car->parking_time);
    state_unlock(h);
    bool full_time = process_sleep(h, car->parking_time);
    state_lock(h);
    record_event(p, full_time ? "Окончание стоянки: авто #%d покидает место #%d" :
                 "Остановка: авто #%d досрочно покидает место #%d", car->id, spot->id);
    spot->car_idx = -1;
    car->status = CAR_DONE;
    record_event(p, "Освобождение места #%d (%s)", spot->id, type_names[spot->type]);
    car_finished(h);
    state_unlock(h);
    sem_post_checked(h->dispatcher_wake);
}

static void render_process(ForkContext *h) {
    do {
        state_lock(h);
        bool stop = h->parking.state->stop;
        int len = 0;
        char *screen = stop ? NULL : build_dashboard(&h->parking, &len);
        state_unlock(h);
        if (stop) return;
        if (screen == NULL) { errno = ENOMEM; report_error("буфер табло"); _exit(2); }
        write_all(STDOUT_FILENO, screen, (size_t)len);
        free(screen);
    } while (process_sleep(h, 1));
}

int block_signals(sigset_t *signals) {
    sigemptyset(signals);
    sigaddset(signals, SIGINT);
    sigaddset(signals, SIGTERM);
    sigaddset(signals, SIGCHLD);
    // Дети наследуют маску, Ctrl+C обрабатывает только родитель через sigtimedwait.
    return sigprocmask(SIG_BLOCK, signals, NULL);
}

static int start_child(ForkContext *h, Child *children, int *count, int role, int car_idx) {
    pid_t pid = fork();
    if (pid < 0) { report_error("fork"); return -1; }
    if (pid == 0) {
        if (role == 0) dispatcher_process(h);
        else if (role == 1) render_process(h);
        else car_process(h, car_idx);
        // Только родитель разрушает общие семафоры и выводит итоговую статистику.
        _exit(close_child_semaphores(h) == 0 ? 0 : 2);
    }
    children[*count] = (Child){ .pid = pid, .car = role == 2 };
    (*count)++;
    return 0;
}

static bool reap_children(Child *children, int count, bool stopping, int *alive) {
    bool failed = false;
    for (int i = 0; i < count; i++) {
        if (children[i].pid == 0) continue;
        int status;
        pid_t rc = waitpid(children[i].pid, &status, WNOHANG);
        if (rc < 0 && errno == EINTR) continue;
        if (rc == 0) continue;
        if (rc < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            (!stopping && !children[i].car)) failed = true;
        children[i].pid = 0;
        (*alive)--;
    }
    return failed;
}

static void emergency_reap(Child *children, int count) {
    // Аварийный ребёнок мог погибнуть внутри state_lock: общие данные больше не трогаем.
    for (int i = 0; i < count; i++) {
        if (children[i].pid > 0) kill(children[i].pid, SIGKILL);
    }
    for (int i = 0; i < count; i++) {
        if (children[i].pid > 0)
            while (waitpid(children[i].pid, NULL, 0) < 0 && errno == EINTR) {}
    }
}

static int parent_lock(ForkContext *h, Child *children, int count, bool stopping, int *alive) {
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += CHECK_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(h->state_lock, &deadline) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) { report_error("sem_timedwait(state_lock)"); return -1; }
        // Не ждём вечно, если ребёнок умер, удерживая бинарный семафор.
        if (reap_children(children, count, stopping, alive)) return -1;
    }
}

int run_processes(ForkContext *h, const sigset_t *signals, const char **reason) {
    Parking *p = &h->parking;
    ParkingState *s = p->state;
    Child *children = calloc((size_t)s->total_cars + 2, sizeof(*children));
    if (children == NULL) { report_error("массив PID"); return 1; }
    int count = 0, alive = 0, result = 0, stop_signal = 0;
    bool stopping = false;
    const char *exit_reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА";
    for (int role = 0; role < 2; role++) {
        if (start_child(h, children, &count, role, -1) < 0) { result = 1; break; }
    }
    alive = count;
    unsigned int rng = s->params.seed;
    int cars_left[3];
    for (int i = 0; i < 3; i++) cars_left[i] = s->params.cars_count[i];
    int interval = get_rand_range(&rng, s->params.arrival_min, s->params.arrival_max);
    long long next_arrival = monotonic_ns() + ticks_ns(&s->clock, interval);
    long long limit = s->params.max_ticks == 0 ? 0 :
        s->clock.start_ns + ticks_ns(&s->clock, s->params.max_ticks);
    while (true) {
        if (reap_children(children, count, stopping, &alive)) {
            goto crashed;
        }
        // Сначала принимаем уже ожидающий сигнал, затем проверяем лимиты и генерируем авто.
        struct timespec zero = {0};
        int sig;
        while ((sig = sigtimedwait(signals, NULL, &zero)) >= 0) {
            if (!stopping && (sig == SIGINT || sig == SIGTERM) && stop_signal == 0)
                stop_signal = sig;
        }
        long long now = monotonic_ns();
        if (!stopping) {
            bool done = sem_trywait(h->sim_end) == 0;
            if (!done && errno != EAGAIN) { report_error("sem_trywait(sim_end)"); result = 1; }
            if (result || stop_signal || done || (limit && now >= limit)) {
                stopping = true;
                if (parent_lock(h, children, count, stopping, &alive) < 0) goto crashed;
                s->stop = true;
                s->generator_done = true;
                if (result) {
                    exit_reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (ошибка запуска)";
                    record_event(p, "Ошибка запуска. Остановка симуляции.");
                } else if (stop_signal) {
                    exit_reason = stop_signal == SIGINT ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" :
                        "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)";
                    record_event(p, "Получен сигнал %s - остановка симуляции",
                                 stop_signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM");
                } else if (!done) {
                    exit_reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)";
                    record_event(p, "Достигнут лимит времени. Завершение.");
                } else record_event(p, "Все автомобили обработаны. Завершение.");
                state_unlock(h);
                sem_post_checked(h->dispatcher_wake);
            }
        }
        if (stopping && alive == 0) break;
        if (!stopping && s->cars_generated < s->total_cars && now >= next_arrival) {
            int available[3], n = 0;
            for (int i = 0; i < 3; i++) if (cars_left[i] > 0) available[n++] = i;
            int type = available[get_rand_range(&rng, 0, n - 1)];
            int duration = get_rand_range(&rng, s->params.park_time_min, s->params.park_time_max);
            if (parent_lock(h, children, count, stopping, &alive) < 0) goto crashed;
            int idx = s->cars_generated++;
            p->cars[idx] = (Car){ .id = idx + 1, .type = (CarType)type,
                .parking_time = duration, .status = CAR_ARRIVING, .spot_idx = -1 };
            record_event(p, "Генерация: авто #%d (%s), интервал %d т., стоянка %d т.",
                         idx + 1, type_names[type], interval, duration);
            state_unlock(h);
            // fork выполняется вне state_lock: ребёнок сразу сможет взять блокировку.
            if (start_child(h, children, &count, 2, idx) < 0) {
                if (parent_lock(h, children, count, stopping, &alive) < 0) goto crashed;
                s->cars_generated--;
                state_unlock(h);
                result = 1;
                continue;
            }
            alive++;
            cars_left[type]--;
            interval = get_rand_range(&rng, s->params.arrival_min, s->params.arrival_max);
            next_arrival = monotonic_ns() + ticks_ns(&s->clock, interval);
        }
        if (!stopping) {
            if (parent_lock(h, children, count, stopping, &alive) < 0) goto crashed;
            if (s->cars_generated == s->total_cars) s->generator_done = true;
            notify_if_all_done(h);
            state_unlock(h);
        }
        now = monotonic_ns();
        long long wake = now + CHECK_NS;
        if (!stopping && s->cars_generated < s->total_cars && next_arrival < wake) wake = next_arrival;
        if (!stopping && limit && limit < wake) wake = limit;
        long long delay = wake > now ? wake - now : 0;
        struct timespec timeout = { .tv_sec = delay / 1000000000LL, .tv_nsec = delay % 1000000000LL };
        sig = sigtimedwait(signals, NULL, &timeout);
        if (!stopping && (sig == SIGINT || sig == SIGTERM) && stop_signal == 0) stop_signal = sig;
        if (sig < 0 && errno != EAGAIN && errno != EINTR) { report_error("sigtimedwait"); result = 1; }
    }
    free(children);
    *reason = exit_reason;
    return result;

crashed:
    {
        const char err[] = "Ошибка: дочерний процесс завершился аварийно\n";
        write_all(STDERR_FILENO, err, sizeof(err) - 1);
    }
    emergency_reap(children, count);
    free(children);
    return 2;
}
