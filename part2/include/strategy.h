#ifndef STRATEGY_H
#define STRATEGY_H

#include "common.h"
#include "queue.h"

// Выбирает из очереди автомобиль типа type, которому отдать освободившееся место.
// Возвращает его индекс в очереди или -1, если подходящих нет. rng нужен для STRATEGY_RANDOM
int pick_car(const Queue *q, CarType type, Strategy strategy, unsigned int *rng);

#endif // STRATEGY_H
