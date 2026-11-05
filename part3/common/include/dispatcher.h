#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "parking.h"

// Способ доставки решения задаёт реализация IPC; все вызовы — под общей блокировкой.
typedef void (*CarNotifier)(void *context, int car_idx);
void dispatch_cars(Parking *p, unsigned int *rng, CarNotifier notify, void *context);
void cancel_waiting(Parking *p, CarNotifier notify, void *context);

#endif
