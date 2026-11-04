#include "process_utils.h"
#include "params.h"
#include "sim_clock.h"
#include "io.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    sigset_t signals;
    if (model_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    SimParams checked;
    if (parse_params(argc, argv, &checked) < 0) return 1;
    char dispatcher[4096], arrivals[4096], id[49];
    if (sibling_executable("parking_dispatcher", dispatcher, sizeof(dispatcher)) < 0 ||
        sibling_executable("parking_arrivals", arrivals, sizeof(arrivals)) < 0) {
        report_error("путь программ сеанса"); return 1;
    }
    snprintf(id, sizeof(id), "run_%ld_%lld", (long)getpid(), monotonic_ns());
    char **args = calloc((size_t)argc + 2, sizeof(*args));
    if (!args) { report_error("аргументы запуска"); return 1; }
    args[0] = dispatcher;
    args[1] = id;
    for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
    pid_t owner = 0, generator = 0;
    int error = posix_spawn(&owner, dispatcher, NULL, NULL, args, environ);
    free(args);
    if (error) { errno = error; report_error("запуск диспетчера"); return 1; }
    char *generator_args[] = { arrivals, id, NULL };
    error = posix_spawn(&generator, arrivals, NULL, NULL, generator_args, environ);
    int result = error ? 1 : 0;
    if (error) { generator = 0; errno = error; report_error("запуск генератора"); kill(owner, SIGTERM); }
    long long failed_since = error ? monotonic_ns() : 0;
    while (owner || generator) {
        int sig = model_stop_signal(&signals);
        if (sig && owner) kill(owner, sig);
        else if (sig && generator) kill(generator, sig);
        pid_t *pids[] = { &owner, &generator };
        for (int i = 0; i < 2; i++) {
            if (!*pids[i]) continue;
            int status;
            pid_t rc = waitpid(*pids[i], &status, WNOHANG);
            if (rc < 0 && errno == EINTR) continue;
            if (rc == 0) continue;
            if (rc < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
                result = 1;
                if (!failed_since) failed_since = monotonic_ns();
                if (*pids[1 - i]) kill(*pids[1 - i], SIGTERM);
            } else if (i == 0 && generator) {
                // Нулевой сеанс может закончиться до подключения генератора.
                kill(generator, SIGTERM);
            }
            *pids[i] = 0;
        }
        if (failed_since && monotonic_ns() - failed_since > 4000000000LL) {
            if (owner) kill(owner, SIGKILL);
            if (generator) kill(generator, SIGKILL);
        }
        if (owner || generator) sleep_until(monotonic_ns() + 10000000LL);
    }
    return result;
}
