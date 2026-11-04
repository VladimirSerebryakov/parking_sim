#ifndef SIM_CLOCK_H
#define SIM_CLOCK_H

#include <time.h>

typedef struct {
    long long start_ns; // Начало симуляции, CLOCK_MONOTONIC
    int tick_ms; // Миллисекунд в такте
} SimClock;

long long monotonic_ns(void);
long long ticks_ns(const SimClock *clock, int ticks);
long long now_ticks(const SimClock *clock);
void deadline_after(const SimClock *clock, int ticks, struct timespec *deadline);
void sleep_until(long long end_ns);
int get_rand_range(unsigned int *seed, int min, int max);

#endif
