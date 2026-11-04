#ifndef STRATEGY_H
#define STRATEGY_H

#include "common.h"
#include "queue.h"

// Индекс в очереди, а не в массиве cars; -1, если совместимых автомобилей нет.
int pick_car(const Queue *q, const int *ids, const Car *cars, CarType type,
             Strategy strategy, unsigned int *rng);

#endif
