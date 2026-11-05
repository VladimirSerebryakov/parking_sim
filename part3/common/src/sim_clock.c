#include "sim_clock.h"
#include <errno.h>
#include <stdlib.h>

#define NS_IN_SEC 1000000000LL

long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NS_IN_SEC + ts.tv_nsec;
}

long long ticks_ns(const SimClock *clock, int ticks) {
    return (long long)ticks * clock->tick_ms * 1000000LL;
}

long long now_ticks(const SimClock *clock) {
    if (clock->start_ns == 0) return 0; // Независимый сеанс ещё ждёт первого участника.
    return (monotonic_ns() - clock->start_ns) / ticks_ns(clock, 1);
}

void deadline_after(const SimClock *clock, int ticks, struct timespec *deadline) {
    clock_gettime(CLOCK_REALTIME, deadline);
    long long ns = deadline->tv_nsec + ticks_ns(clock, ticks);
    deadline->tv_sec += ns / NS_IN_SEC;
    deadline->tv_nsec = ns % NS_IN_SEC;
}

void sleep_until(long long end_ns) {
    struct timespec ts = { .tv_sec = end_ns / NS_IN_SEC, .tv_nsec = end_ns % NS_IN_SEC };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

int get_rand_range(unsigned int *seed, int min, int max) {
    if (min == max) return min;
    return min + (int)((unsigned int)rand_r(seed) % ((unsigned int)max - min + 1U));
}
