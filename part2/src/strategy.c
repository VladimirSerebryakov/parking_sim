#include "strategy.h"
#include "sim_clock.h"

int pick_car(const Queue *q, CarType type, Strategy strategy, unsigned int *rng) {
    // один проход по очереди: первый, последний и самый короткий из подходящих по типу
    int first = -1, last = -1, shortest = -1, count = 0;
    for (int i = 0; i < q->size; i++) {
        const Car *car = queue_get(q, i);
        if (car->type != type) continue;
        if (first == -1) first = i;
        last = i;
        if (shortest == -1 || car->parking_time < queue_get(q, shortest)->parking_time) shortest = i;
        count++;
    }
    if (count == 0) return -1;

    switch (strategy) {
        case STRATEGY_LIFO:
            return last;
        case STRATEGY_SHORT:
            return shortest;
        case STRATEGY_RANDOM: {
            // выбираем k-ю по счёту подходящую машину
            int k = get_rand_range(rng, 0, count - 1);
            for (int i = 0; i < q->size; i++) {
                if (queue_get(q, i)->type == type && k-- == 0) return i;
            }
            return first;
        }
        case STRATEGY_FIFO:
        default:
            return first;
    }
}
