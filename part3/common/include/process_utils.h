#ifndef PROCESS_UTILS_H
#define PROCESS_UTILS_H
#include <signal.h>
#include <stddef.h>
int model_block_signals(sigset_t *signals);
int model_stop_signal(const sigset_t *signals);
int sibling_executable(const char *name, char *path, size_t size);
#endif
