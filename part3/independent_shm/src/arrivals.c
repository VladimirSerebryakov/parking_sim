#include "session.h"
#include "io.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int generate(Session *s, const char *id, const sigset_t *signals) {
    ParkingState *state = s->memory.parking.state;
    pid_t *children = calloc((size_t)state->total_cars + 1, sizeof(*children));
    if (!children) return -1;
    char executable[4096];
    if (sibling_executable("parking_car", executable, sizeof(executable)) < 0) { free(children); return -1; }
    int next = 0, spawned = 0, alive = 0, result = 0;
    long long arrival = monotonic_ns();
    if (state->total_cars) arrival += ticks_ns(&state->clock, s->slots[0].interval);
    for (;;) {
        int sig = session_signal(signals);
        if (sig && session_request_stop(s, sig, false) < 0) { result = -1; break; }
        if (!session_owner_alive(s)) { errno = EPIPE; result = -1; break; }
        if (session_lock(s) < 0) { result = -1; break; }
        bool stop = state->stop;
        session_unlock(s);
        bool child_failed = false;
        for (int i = 0; i < spawned; i++) {
            if (!children[i]) continue;
            int status;
            pid_t rc = waitpid(children[i], &status, WNOHANG);
            if (rc < 0 && errno == EINTR) continue;
            if (!rc) continue;
            if (rc < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) child_failed = true;
            children[i] = 0;
            alive--;
        }
        if (child_failed && !stop) {
            result = -1;
            if (session_request_stop(s, 0, true) < 0) break;
            stop = true;
        }
        long long now = monotonic_ns();
        if (!stop && next < state->total_cars && now >= arrival) {
            if (session_lock(s) < 0) { result = -1; break; }
            bool seen = s->slots[next].seen;
            session_unlock(s);
            if (!seen) {
                char number[32];
                snprintf(number, sizeof(number), "%d", next + 1);
                char *args[] = { executable, (char *)id, number, NULL };
                int error = posix_spawn(&children[spawned], executable, NULL, NULL, args, environ);
                if (error) {
                    errno = error;
                    report_error("posix_spawn автомобиля");
                    result = -1;
                    if (session_request_stop(s, 0, true) < 0) break;
                    stop = true;
                } else { spawned++; alive++; }
            }
            next++;
            if (next < state->total_cars)
                arrival = monotonic_ns() + ticks_ns(&state->clock, s->slots[next].interval);
        }
        if ((stop || next == state->total_cars) && alive == 0) break;
        now = monotonic_ns();
        long long wake = now + 10000000LL;
        if (!stop && next < state->total_cars && arrival < wake) wake = arrival;
        if (wake > now) sleep_until(wake);
    }
    // На аварийном пути собираем именно собственных детей; независимые программы не трогаем.
    for (int i = 0; i < spawned; i++) if (children[i]) kill(children[i], SIGKILL);
    for (int i = 0; i < spawned; i++) if (children[i])
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    free(children);
    return result;
}

int main(int argc, char **argv) {
    sigset_t signals;
    if (session_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc != 2) { errno = EINVAL; report_error("использование: parking_arrivals СЕАНС"); return 1; }
    Session session;
    if (session_connect(&session, argv[1], -1) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение генератора"); return 1;
    }
    int rc = generate(&session, argv[1], &signals);
    if (rc < 0) session_request_stop(&session, 0, true);
    if (session_disconnect(&session) < 0) rc = -1;
    session_close(&session);
    return rc == 0 ? 0 : 1;
}
