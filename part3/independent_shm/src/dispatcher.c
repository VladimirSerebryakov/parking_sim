#include "session.h"
#include "dispatcher.h"
#include "io.h"
#include "ui.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool missing_client(Session *s) {
    bool missing = false;
    for (int i = 0; i < s->memory.parking.state->total_cars; i++) {
        if (s->slots[i].pid > 0 && kill(s->slots[i].pid, 0) < 0 && errno == ESRCH) {
            s->slots[i].pid = 0;
            s->meta->clients--;
            missing = true;
        }
    }
    if (s->meta->generator_pid > 0 && kill(s->meta->generator_pid, 0) < 0 && errno == ESRCH) {
        s->meta->generator_pid = 0;
        s->meta->clients--;
        missing = true;
    }
    return missing;
}

static int serve(Session *s, const sigset_t *signals, const char **reason) {
    Parking *p = &s->memory.parking;
    unsigned int rng = p->state->params.seed + 1U;
    long long frame = 0, stopping_since = 0;
    bool bad = false;
    for (;;) {
        int sig = session_signal(signals);
        if (session_lock(s) < 0) return -1;
        if (sig && !s->meta->stop_signal) s->meta->stop_signal = sig;
        if (missing_client(s)) s->meta->failed = true;
        long long now = monotonic_ns();
        bool done = p->state->total_cars == 0 ||
            (s->meta->started && p->state->cars_finished == p->state->total_cars);
        bool limit = s->meta->started && p->state->params.max_ticks > 0 &&
            now >= p->state->clock.start_ns + ticks_ns(&p->state->clock, p->state->params.max_ticks);
        if (!p->state->stop && (s->meta->stop_signal || s->meta->failed || done || limit)) {
            p->state->stop = true;
            s->meta->closing = true;
            stopping_since = now;
            if (s->meta->failed) {
                bad = true;
                record_event(p, "Ошибка участника. Остановка сеанса.");
            } else if (s->meta->stop_signal) {
                *reason = s->meta->stop_signal == SIGINT ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" :
                    "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)";
                record_event(p, "Получен сигнал %s - остановка симуляции",
                             s->meta->stop_signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM");
            } else if (limit && !done) {
                *reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)";
                record_event(p, "Достигнут лимит времени. Завершение.");
            } else record_event(p, "Все автомобили обработаны. Завершение.");
            cancel_waiting(p, session_notify, s);
        }
        if (s->meta->failed) bad = true;
        if (!p->state->stop) dispatch_cars(p, &rng, session_notify, s);
        int clients = s->meta->clients;
        bool stopping = p->state->stop;
        int len = 0;
        char *screen = now >= frame ? build_dashboard(p, &len) : NULL;
        session_unlock(s);
        if (now >= frame && !screen) { errno = ENOMEM; return -1; }
        if (screen) {
            write_all(STDOUT_FILENO, screen, (size_t)len);
            free(screen);
            frame = now + ticks_ns(&p->state->clock, 1);
        }
        if (stopping && clients == 0) return bad ? -1 : 0;
        if (stopping && now - stopping_since > 3000000000LL) { errno = ETIMEDOUT; return -1; }
        struct timespec wake;
        clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_nsec += 10000000L;
        if (wake.tv_nsec >= 1000000000L) { wake.tv_sec++; wake.tv_nsec -= 1000000000L; }
        int rc;
        do { rc = sem_timedwait(s->wake, &wake); } while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != ETIMEDOUT) return -1;
    }
}

int main(int argc, char **argv) {
    sigset_t signals;
    if (session_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc < 2) { errno = EINVAL; report_error("нужно имя сеанса"); return 1; }
    const char *id = argv[1];
    SimParams params;
    if (parse_params(argc - 1, argv + 1, &params) < 0) return 1;
    if (params.log_path[0] != '/') {
        char cwd[4096], path[4096];
        if (!getcwd(cwd, sizeof(cwd))) { report_error("getcwd"); return 1; }
        int len = snprintf(path, sizeof(path), "%s/%s", cwd, params.log_path);
        if (len < 0 || (size_t)len >= sizeof(path)) { errno = ENAMETOOLONG; report_error("путь журнала"); return 1; }
        strcpy(params.log_path, path);
    }
    Session session;
    if (session_create(&session, id, &params) < 0) { report_error("создание сеанса"); return 1; }
    Parking *p = &session.memory.parking;
    ui_enter();
    const char *reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА";
    int rc = serve(&session, &signals, &reason);
    if (rc < 0) {
        const char error[] = "Ошибка: сеанс остановлен из-за ошибки участника или синхронизации\n";
        write_all(STDERR_FILENO, error, sizeof(error) - 1);
    } else draw_dashboard(p);
    ui_leave();
    if (rc == 0) print_final_stats(p, reason);
    session_close(&session);
    return rc == 0 ? 0 : 1;
}
