#include "client.h"
#include "process_utils.h"
#include "sim_clock.h"
#include "io.h"
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int generate(Client *c, const char *host, const char *port) {
    pid_t *children = calloc((size_t)c->total + 1, sizeof(*children));
    if (!children) return -1;
    char executable[4096];
    if (sibling_executable("parking_car", executable, sizeof(executable)) < 0) { free(children); return -1; }
    int next = 1, alive = 0, result = 0, pending = 0;
    bool requested = false;
    long long arrival_at = 0;
    while (!tcp_interrupted && !c->stopped) {
        // Собираем только своих детей; исчезновение клиента не останавливает сервер.
        for (int id = 1; id <= c->total; id++) if (children[id]) {
            int status;
            pid_t rc = waitpid(children[id], &status, WNOHANG);
            if (rc < 0 && errno == EINTR) continue;
            if (rc) {
                if (rc < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) result = -1;
                children[id] = 0; alive--;
            }
        }
        if (pending && monotonic_ns() >= arrival_at) {
            char number[32]; snprintf(number, sizeof(number), "%d", pending);
            char *args[] = { executable, (char *)host, (char *)port, number, NULL };
            pid_t child;
            int error = posix_spawn(&child, executable, NULL, NULL, args, environ);
            if (error) { errno = error; result = -1; break; }
            children[pending] = child; // PID учитывается только после успешного запуска.
            char message[128]; snprintf(message, sizeof(message), "Генератор: запущен клиент авто #%d\n", pending); client_print(message);
            alive++; pending = 0; next++;
        }
        if (next > c->total && !alive) break;
        if (!pending && !requested && next <= c->total) {
            char number[32]; snprintf(number, sizeof(number), "%d", next);
            if (client_send(c, NET_PLAN, number) < 0) { result = -1; break; }
            requested = true;
        }
        NetFrame f;
        int rc = client_next(c, &f, 10);
        if (rc < 0) { result = -1; break; }
        if (rc == 2) break;
        if (rc == 1) {
            long long v[5];
            if (!requested || f.type != NET_PLAN_REPLY || text_numbers(f.body, v, 5) < 0 ||
                v[0] != next || v[1] < 0 || v[1] > 2147483647 ||
                v[1] > LLONG_MAX / 2 / 1000000LL / c->tick_ms ||
                v[2] < 0 || v[2] > 1 || v[3] < 0 || v[3] > 1 || v[4] < 0 || v[4] > 1) {
                errno = EPROTO; result = -1;
            } else {
                requested = false;
                if (v[3] || v[4]) next++; // Завершённые или уже управляемые номера пропускаются.
                else {
                    pending = next;
                    arrival_at = monotonic_ns() + (v[2] ? 0 : v[1] * (long long)c->tick_ms * 1000000LL);
                }
            }
            net_frame_free(&f);
            if (result < 0) break;
        }
    }
    // Генератор отключается отдельно, останавливая только созданные им локальные процессы.
    for (int id = 1; id <= c->total; id++) if (children[id]) kill(children[id], SIGTERM);
    long long end = monotonic_ns() + 2000000000LL;
    while (alive && monotonic_ns() < end) {
        for (int id = 1; id <= c->total; id++) if (children[id]) {
            pid_t rc = waitpid(children[id], NULL, WNOHANG);
            if (rc > 0 || (rc < 0 && errno == ECHILD)) { children[id] = 0; alive--; }
        }
        if (alive) sleep_until(monotonic_ns() + 10000000LL);
    }
    for (int id = 1; id <= c->total; id++) if (children[id]) kill(children[id], SIGKILL);
    for (int id = 1; id <= c->total; id++) if (children[id])
        while (waitpid(children[id], NULL, 0) < 0 && errno == EINTR) {}
    free(children);
    return result;
}
int main(int argc, char **argv) {
    if (tcp_signals() < 0) { report_error("sigaction"); return 1; }
    if (argc != 3) { client_print("Использование: parking_arrivals АДРЕС ПОРТ\n"); return 1; }
    Client c;
    if (client_open(&c, argv[1], argv[2], ROLE_GENERATOR, 0) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение генератора"); return 1;
    }
    c.defer_stop_ack = true;
    int rc = generate(&c, argv[1], argv[2]);
    if (c.stopped) client_ack_stop(&c);
    if (rc < 0) report_error("генератор прибытий");
    else client_detach(&c);
    client_close(&c);
    return rc == 0 ? 0 : 1;
}
