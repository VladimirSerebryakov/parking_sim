#include "traffic.h"
#include "sim_clock.h"
void traffic_init(Traffic *t, const SimParams *params) {
    t->rng = params->seed;
    for (int i = 0; i < 3; i++) t->left[i] = params->cars_count[i];
}
void traffic_next(Traffic *t, const SimParams *p, int id, Car *car, int *interval) {
    *interval = get_rand_range(&t->rng, p->arrival_min, p->arrival_max);
    int types[3], n = 0;
    for (int i = 0; i < 3; i++) if (t->left[i]) types[n++] = i;
    int type = types[get_rand_range(&t->rng, 0, n - 1)];
    t->left[type]--;
    *car = (Car){ .id = id, .type = (CarType)type, .status = CAR_ARRIVING, .spot_idx = -1,
                 .parking_time = get_rand_range(&t->rng, p->park_time_min, p->park_time_max) };
}
