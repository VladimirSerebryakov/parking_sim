#include "strategy.h"
#include "sim_clock.h"

int pick_car(const Queue *q, const int *ids, const Car *cars, CarType type,
             Strategy strategy, unsigned int *rng) {
    int first = -1, last = -1, shortest = -1, count = 0;
    for (int i = 0; i < q->size; i++) {
        const Car *car = &cars[queue_get(q, ids, i)];
        if (car->type != type) continue;
        if (first == -1) first = i;
        last = i;
        if (shortest == -1 || car->parking_time < cars[queue_get(q, ids, shortest)].parking_time)
            shortest = i;
        count++;
    }
    if (count == 0) return -1;
    if (strategy == STRATEGY_LIFO) return last;
    if (strategy == STRATEGY_SHORT) return shortest;
    if (strategy == STRATEGY_RANDOM) {
        int k = get_rand_range(rng, 0, count - 1);
        for (int i = 0; i < q->size; i++) {
            if (cars[queue_get(q, ids, i)].type == type && k-- == 0) return i;
        }
    }
    return first;
}
