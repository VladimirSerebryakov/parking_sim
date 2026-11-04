#ifndef FORK_PROCESSES_H
#define FORK_PROCESSES_H

#include "fork_sync.h"
#include <signal.h>

int block_signals(sigset_t *signals);
// 0 — штатная остановка, 1 — ошибка запуска, 2 — авария ребёнка; все дети уже собраны.
int run_processes(ForkContext *ctx, const sigset_t *signals, const char **reason);

#endif
