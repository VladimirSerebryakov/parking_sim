#ifndef SIM_CLOCK_H
#define SIM_CLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

// Запоминает момент старта симуляции и длительность такта
void sim_clock_start(int tick_ms);
// Сколько полных тактов прошло с момента старта
int now_ticks(void);
// Спит заданное число тактов. Если stop != NULL, каждые SLEEP_STEP_MS проверяет флаг
// и при остановке просыпается досрочно. Возвращает false, если сон прерван остановкой
bool sleep_ticks(int ticks, const atomic_bool *stop);
// Абсолютный момент времени через ticks тактов (CLOCK_REALTIME, для sem_timedwait)
void deadline_after(int ticks, struct timespec *ts);
// Случайное число из [min, max]; seed - собственное состояние ГСЧ вызывающего (rand_r)
int get_rand_range(unsigned int *seed, int min, int max);

#endif // SIM_CLOCK_H
