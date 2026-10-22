#include "sim_clock.h"
#include <stdlib.h>

#define SLEEP_STEP_MS 50 // Шаг, с которым sleep_ticks проверяет флаг остановки
#define NS_IN_MS 1000000LL
#define NS_IN_SEC 1000000000LL

static struct timespec start_time; // Момент старта симуляции (CLOCK_MONOTONIC)
static int tick_len_ms = 800; // Длительность такта в мс

static long long to_ns(const struct timespec *ts) {
    return ts->tv_sec * NS_IN_SEC + ts->tv_nsec;
}

static struct timespec from_ns(long long ns) {
    struct timespec ts = { .tv_sec = ns / NS_IN_SEC, .tv_nsec = ns % NS_IN_SEC };
    return ts;
}

static long long clock_now_ns(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return to_ns(&ts);
}

void sim_clock_start(int tick_ms) {
    tick_len_ms = tick_ms;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
}

int now_ticks(void) {
    long long elapsed_ns = clock_now_ns(CLOCK_MONOTONIC) - to_ns(&start_time);
    return (int)(elapsed_ns / (tick_len_ms * NS_IN_MS));
}

bool sleep_ticks(int ticks, const atomic_bool *stop) {
    // спим до абсолютного момента end короткими шагами, чтобы не накапливалась погрешность
    long long end = clock_now_ns(CLOCK_MONOTONIC) + (long long)ticks * tick_len_ms * NS_IN_MS;
    for (;;) {
        if (stop != NULL && atomic_load(stop)) return false;
        long long now = clock_now_ns(CLOCK_MONOTONIC);
        if (now >= end) return true;
        long long step_end = now + SLEEP_STEP_MS * NS_IN_MS;
        struct timespec wake = from_ns(step_end < end ? step_end : end);
        // при прерывании сигналом (EINTR) просто идём на следующую итерацию
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
    }
}

void deadline_after(int ticks, struct timespec *ts) {
    *ts = from_ns(clock_now_ns(CLOCK_REALTIME) + (long long)ticks * tick_len_ms * NS_IN_MS);
}

// Генерация рандомного числа
int get_rand_range(unsigned int *seed, int min, int max) {
    if (min == max) return min;
    return min + rand_r(seed) % (max - min + 1);
}
