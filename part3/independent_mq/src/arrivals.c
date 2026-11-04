#include "protocol.h"
#include "traffic.h"
#include "io.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int generate(MqClient *c, const char *id, const sigset_t *signals) {
    SimParams params = c->welcome.params;
    int total = params.cars_count[0] + params.cars_count[1] + params.cars_count[2];
    pid_t *children = calloc((size_t)total + 1, sizeof(*children));
    if (!children) return -1;
    char executable[4096];
    if (sibling_executable("parking_car", executable, sizeof(executable)) < 0) { free(children); return -1; }
    Traffic traffic; traffic_init(&traffic, &params);
    Car plan; int interval = 0;
    if (total) traffic_next(&traffic, &params, 1, &plan, &interval);
    long long arrival = monotonic_ns() + ticks_ns(&c->welcome.clock, interval);
    int spawned = 0, alive = 0, result = 0;
    for (;;) {
        int sig = model_stop_signal(signals);
        if (sig && client_call(c, MQ_STOP_REQUEST, sig, NULL) < 0) { result = -1; break; }
        int rc = client_poll(c);
        if (rc < 0 || (rc == 0 && !transport_alive(&c->transport))) { result = -1; break; }
        bool failed = false;
        for (int i = 0; i < spawned; i++) {
            if (!children[i]) continue;
            int status;
            pid_t pid = waitpid(children[i], &status, WNOHANG);
            if (pid < 0 && errno == EINTR) continue;
            if (!pid) continue;
            if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) failed = true;
            children[i] = 0; alive--;
        }
        if (failed && !c->stop) {
            result = -1;
            if (client_call(c, MQ_FAIL, 0, NULL) < 0) break;
            c->stop = true;
        }
        if (!c->stop && spawned < total && monotonic_ns() >= arrival) {
            char number[32]; snprintf(number, sizeof(number), "%d", spawned + 1);
            char *args[] = { executable, (char *)id, number, NULL };
            int error = posix_spawn(&children[spawned], executable, NULL, NULL, args, environ);
            if (error) {
                errno = error; report_error("posix_spawn MQ-автомобиля"); result = -1;
                if (client_call(c, MQ_FAIL, 0, NULL) < 0) break;
                c->stop = true;
            } else {
                spawned++; alive++;
                if (spawned < total) traffic_next(&traffic, &params, spawned + 1, &plan, &interval);
                arrival = monotonic_ns() + ticks_ns(&c->welcome.clock, interval);
            }
        }
        if ((c->stop || spawned == total) && alive == 0) break;
        sleep_until(monotonic_ns() + 1000000LL);
    }
    for (int i = 0; i < spawned; i++) if (children[i]) kill(children[i], SIGKILL);
    for (int i = 0; i < spawned; i++) if (children[i])
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    free(children);
    if (client_disconnect(c) < 0) result = -1;
    return result;
}
int main(int argc, char **argv) {
    sigset_t signals;
    if (model_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc != 2) { errno = EINVAL; report_error("использование: parking_arrivals СЕАНС"); return 1; }
    MqClient client;
    if (client_connect(&client, argv[1], 0) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение MQ-генератора"); return 1;
    }
    int rc = generate(&client, argv[1], &signals);
    client_close(&client);
    return rc == 0 ? 0 : 1;
}
