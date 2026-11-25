#include "dispatcher.h"
#include "strategy.h"

static void assign_spot(Parking *p, CarNotifier notify, void *context, int car_idx, int spot_idx, bool from_queue) {
    Car *car = &p->cars[car_idx];
    ParkingSpot *spot = &p->spots[spot_idx];
    spot->car_idx = car_idx;
    car->spot_idx = spot_idx;
    car->leave_tick = now_ticks(&p->state->clock) + car->parking_time;
    car->status = CAR_PARKED;
    p->state->cars_served++;
    if (from_queue)
        record_event(p, "Диспетчер: место #%d назначено авто #%d из очереди (ждало %lld т.)",
                     spot->id, car->id, now_ticks(&p->state->clock) - car->arrival_tick);
    else
        record_event(p, "Диспетчер: авто #%d назначено место #%d", car->id, spot->id);
    // Статус и уведомление — в одной критической секции, в том числе при гонке таймаута.
    notify(context, car_idx);
}

static void serve_queue(Parking *p, CarNotifier notify, void *context, unsigned int *rng) {
    for (int i = 0; i < p->state->total_spots && p->state->queue.size > 0; i++) {
        if (p->spots[i].car_idx != -1) continue;
        int qidx = pick_car(&p->state->queue, p->queue_ids, p->cars,
                            p->spots[i].type, p->state->params.strategy, rng);
        if (qidx < 0) continue;
        int car_idx = queue_get(&p->state->queue, p->queue_ids, qidx);
        queue_remove_at(&p->state->queue, p->queue_ids, qidx);
        assign_spot(p, notify, context, car_idx, i, true);
    }
}

static void serve_inbox(Parking *p, CarNotifier notify, void *context) {
    int idx;
    while (dequeue(&p->state->inbox, p->inbox_ids, &idx)) {
        Car *car = &p->cars[idx];
        int spot = find_free_spot(p, car->type);
        if (spot >= 0) {
            assign_spot(p, notify, context, idx, spot, false);
            continue;
        }
        if (enqueue(&p->state->queue, p->queue_ids, idx)) {
            car->status = CAR_QUEUED;
            record_event(p, "Диспетчер: мест нет, авто #%d поставлено в очередь (%d/%d)",
                         car->id, p->state->queue.size, p->state->queue.capacity);
        } else {
            car->status = CAR_REJECTED;
            p->state->cars_rejected++;
            record_event(p, "Диспетчер: очередь заполнена, авто #%d уезжает", car->id);
        }
        notify(context, idx);
    }
}

void cancel_waiting(Parking *p, CarNotifier notify, void *context) {
    int idx;
    while (dequeue(&p->state->inbox, p->inbox_ids, &idx) ||
           dequeue(&p->state->queue, p->queue_ids, &idx)) {
        p->cars[idx].status = CAR_CANCELLED;
        p->state->cars_cancelled++;
        record_event(p, "Диспетчер: остановка, ожидание авто #%d прервано", p->cars[idx].id);
        notify(context, idx);
    }
}

void dispatch_cars(Parking *p, unsigned int *rng, CarNotifier notify, void *context) {
    serve_queue(p, notify, context, rng);
    serve_inbox(p, notify, context);
}
