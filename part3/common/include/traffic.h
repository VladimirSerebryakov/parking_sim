#ifndef TRAFFIC_H
#define TRAFFIC_H
#include "params.h"
typedef struct {
    unsigned int rng; // Состояние ГСЧ
    int left[3]; // Остаток машин по типам
} Traffic;
void traffic_init(Traffic *t, const SimParams *params);
void traffic_next(Traffic *t, const SimParams *params, int id, Car *car, int *interval);
#endif
