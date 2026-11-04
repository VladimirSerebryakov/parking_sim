#include "process_utils.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

int model_block_signals(sigset_t *signals) {
    sigemptyset(signals);
    sigaddset(signals, SIGINT);
    sigaddset(signals, SIGTERM);
    sigaddset(signals, SIGCHLD);
    return sigprocmask(SIG_BLOCK, signals, NULL);
}

int model_stop_signal(const sigset_t *signals) {
    struct timespec zero = {0};
    int sig, first = 0;
    while ((sig = sigtimedwait(signals, NULL, &zero)) >= 0)
        if (!first && (sig == SIGINT || sig == SIGTERM)) first = sig;
    return first;
}

int sibling_executable(const char *name, char *path, size_t size) {
    ssize_t len = readlink("/proc/self/exe", path, size - 1);
    if (len < 0) return -1;
    if ((size_t)len == size - 1) { errno = ENAMETOOLONG; return -1; }
    path[len] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash || strlen(name) >= size - (size_t)(slash + 1 - path)) { errno = ENAMETOOLONG; return -1; }
    strcpy(slash + 1, name);
    return 0;
}
