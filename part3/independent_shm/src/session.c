#include "session.h"
#include "process_utils.h"
#include "io.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(SessionMeta) % _Alignof(CarSlot) == 0, "Выравнивание слотов");

static void init_session(Session *s) {
    *s = (Session){ .memory = { .parking = { .log_fd = -1 } }, .car_idx = -1 };
}

int session_name(const char *id, char *name, size_t size) {
    if (strlen(id) == 0 || strlen(id) > 48) { errno = EINVAL; return -1; }
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!isalnum(*p) || *p > 127) {
            if (*p != '_' && *p != '-') { errno = EINVAL; return -1; }
        }
    int len = snprintf(name, size, "/parking_shm_%s", id);
    if (len < 0 || (size_t)len >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static void sem_name(const Session *s, size_t index, char name[128]) {
    snprintf(name, 128, "%s_sem_%zu", s->name, index);
}

static int bind_meta(Session *s) {
    size_t extra = s->memory.shared->extra_offset;
    size_t count = (size_t)s->memory.parking.state->total_cars;
    if (extra > s->memory.size || sizeof(SessionMeta) > s->memory.size - extra ||
        count > (s->memory.size - extra - sizeof(SessionMeta)) / sizeof(CarSlot)) {
        errno = EINVAL;
        return -1;
    }
    s->meta = (SessionMeta *)((char *)s->memory.shared + extra);
    s->slots = (CarSlot *)(s->meta + 1);
    return 0;
}

int session_lock(Session *s) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2; // Ограниченный захват: гибель владельца семафора не создаёт вечного ожидания.
    int rc;
    do { rc = sem_timedwait(s->lock, &deadline); } while (rc < 0 && errno == EINTR);
    return rc;
}

int session_unlock(Session *s) { return sem_post(s->lock); }
int session_wake(Session *s) { return sem_post(s->wake); }

int session_create(Session *s, const char *id, const SimParams *params) {
    init_session(s);
    if (session_name(id, s->name, sizeof(s->name)) < 0) return -1;
    size_t cars = (size_t)params->cars_count[0] + params->cars_count[1] + params->cars_count[2];
    if (cars > (SIZE_MAX - sizeof(SessionMeta)) / sizeof(CarSlot)) { errno = EOVERFLOW; return -1; }
    if (create_session_memory(&s->memory, params, s->name, sizeof(SessionMeta) + cars * sizeof(CarSlot)) < 0)
        return -1;
    s->owner = true;
    if (bind_meta(s) < 0) goto fail;
    s->memory.parking.state->clock.start_ns = 0;
    s->meta->owner_pid = getpid();
    s->owned_sems = calloc(cars + 2, sizeof(*s->owned_sems));
    s->owned_names = calloc(cars + 2, sizeof(*s->owned_names));
    if (!s->owned_sems || !s->owned_names) goto fail;
    for (size_t i = 0; i < cars + 2; i++) {
        char name[128];
        sem_name(s, i, name);
        sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0U);
        if (sem == SEM_FAILED) goto fail;
        s->owned_sems[i] = sem;
        s->owned_names[i] = true;
        s->created++;
    }
    s->lock = s->owned_sems[0]; // Начальное значение 0: подключение ждёт полной инициализации.
    s->wake = s->owned_sems[1];
    unsigned int rng = params->seed;
    int left[3];
    for (int i = 0; i < 3; i++) left[i] = params->cars_count[i];
    for (size_t i = 0; i < cars; i++) {
        int interval = get_rand_range(&rng, params->arrival_min, params->arrival_max);
        int available[3], n = 0;
        for (int t = 0; t < 3; t++) if (left[t]) available[n++] = t;
        int type = available[get_rand_range(&rng, 0, n - 1)];
        left[type]--;
        s->slots[i].interval = interval;
        s->memory.parking.cars[i] = (Car){ .id = (int)i + 1, .type = (CarType)type,
            .parking_time = get_rand_range(&rng, params->park_time_min, params->park_time_max),
            .spot_idx = -1, .status = CAR_ARRIVING };
    }
    Parking *p = &s->memory.parking;
    record_event(p, "--- Старт симуляции (seed: %u, стратегия: %s) ---", params->seed, strategy_name(params->strategy));
    record_event(p, "POSIX память: %s (независимые программы)", s->name);
    record_event(p, "Параметры: места %d/%d/%d; очередь %d", params->spots_count[0],
                 params->spots_count[1], params->spots_count[2], params->queue_capacity);
    s->meta->magic = SESSION_MAGIC;
    if (session_unlock(s) < 0) goto fail;
    return 0;

fail:
    {
        int saved = errno;
        session_close(s);
        errno = saved;
        return -1;
    }
}

int session_connect(Session *s, const char *id, int car_idx) {
    init_session(s);
    s->car_idx = car_idx;
    s->generator = car_idx < 0;
    if (session_name(id, s->name, sizeof(s->name)) < 0) return -1;
    char name[128];
    sem_name(s, 0, name);
    long long end = monotonic_ns() + 5000000000LL;
    do {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)) { errno = ECANCELED; goto fail; }
        s->lock = sem_open(name, 0);
        if (s->lock != SEM_FAILED) break;
        s->lock = NULL;
        if (errno != ENOENT) return -1;
        sleep_until(monotonic_ns() + 10000000LL);
    } while (monotonic_ns() < end);
    if (!s->lock) { errno = ETIMEDOUT; return -1; }
    if (session_lock(s) < 0) goto fail;
    if (attach_shared_memory(&s->memory, s->name) < 0 || bind_meta(s) < 0) goto unlock_fail;
    if (s->meta->closing) { errno = ECANCELED; goto unlock_fail; }
    if (s->meta->magic != SESSION_MAGIC ||
        car_idx >= s->memory.parking.state->total_cars ||
        (s->generator ? s->meta->generator_pid != 0 : s->slots[car_idx].seen)) {
        errno = EBUSY;
        goto unlock_fail;
    }
    sem_name(s, 1, name);
    s->wake = sem_open(name, 0);
    if (s->wake == SEM_FAILED) { s->wake = NULL; goto unlock_fail; }
    if (!s->generator) {
        sem_name(s, (size_t)car_idx + 2, name);
        s->grant = sem_open(name, 0);
        if (s->grant == SEM_FAILED) { s->grant = NULL; goto unlock_fail; }
        s->slots[car_idx].seen = true;
        s->slots[car_idx].pid = getpid();
    } else s->meta->generator_pid = getpid();
    s->meta->clients++;
    s->attached = true;
    if (!s->meta->started) {
        s->meta->started = true;
        s->memory.parking.state->clock.start_ns = monotonic_ns();
    }
    session_unlock(s);
    session_wake(s);
    return 0;

unlock_fail:
    {
        int saved = errno;
        session_unlock(s);
        errno = saved;
    }
fail:
    {
        int saved = errno;
        session_close(s);
        errno = saved;
        return -1;
    }
}

int session_disconnect(Session *s) {
    if (!s->attached) return 0;
    if (session_lock(s) < 0) return -1;
    s->meta->clients--;
    if (s->generator) s->meta->generator_pid = 0;
    else s->slots[s->car_idx].pid = 0;
    s->attached = false;
    session_unlock(s);
    session_wake(s);
    return 0;
}

void session_unlink(Session *s) {
    if (!s->owner) return;
    for (size_t i = 0; i < s->created; i++) {
        if (!s->owned_names[i]) continue;
        char name[128];
        sem_name(s, i, name);
        int rc = sem_unlink(name);
        if (rc < 0 && errno != ENOENT) rc = sem_unlink(name);
        if (rc == 0 || errno == ENOENT) s->owned_names[i] = false;
        else report_error("sem_unlink сеанса");
    }
    if (unlink_shared_memory(&s->memory) < 0) report_error("shm_unlink сеанса");
}

void session_close(Session *s) {
    session_unlink(s);
    if (s->owner) {
        for (size_t i = 0; i < s->created; i++) sem_close(s->owned_sems[i]);
    } else {
        if (s->grant) sem_close(s->grant);
        if (s->wake) sem_close(s->wake);
        if (s->lock) sem_close(s->lock);
    }
    free(s->owned_names);
    free(s->owned_sems);
    destroy_shared_memory(&s->memory);
}

int session_block_signals(sigset_t *signals) { return model_block_signals(signals); }
int session_signal(const sigset_t *signals) { return model_stop_signal(signals); }

int session_request_stop(Session *s, int sig, bool failed) {
    if (session_lock(s) < 0) return -1;
    if (sig && !s->meta->stop_signal) s->meta->stop_signal = sig;
    if (failed) s->meta->failed = true;
    session_unlock(s);
    return session_wake(s);
}

bool session_owner_alive(const Session *s) {
    return kill(s->meta->owner_pid, 0) == 0 || errno == EPERM;
}

void session_notify(void *context, int car_idx) {
    Session *s = context;
    if (sem_post(s->owned_sems[(size_t)car_idx + 2]) < 0) {
        s->meta->failed = true;
        report_error("sem_post автомобилю");
    }
}

