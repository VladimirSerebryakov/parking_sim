#include "parking.h"
#include "sim_clock.h"
#include <stdlib.h>
#include <stdarg.h>

// инициализируем парковку (заполняем параметрами)
void init_parking(Parking *p, const SimParams *params) {
    pthread_mutex_init(&p->mx, NULL);
    sem_init(&p->dispatcher_wake, 0, 0);
    sem_init(&p->sim_end, 0, 0);
    p->end_notified = false;
    p->params = *params;
    p->cars_served = 0;
    p->cars_left_queue = 0;
    p->cars_rejected = 0;
    p->cars_cancelled = 0;
    p->total_spots = params->spots_count[TYPE_REGULAR] +
                     params->spots_count[TYPE_LARGE] +
                     params->spots_count[TYPE_SPECIAL];
    p->spots = (ParkingSpot *)malloc(p->total_spots * sizeof(ParkingSpot));

    int spot_idx = 0;
    for (int type = 0; type < 3; type++) {
        for (int i = 0; i < params->spots_count[type]; i++) {
            p->spots[spot_idx].id = spot_idx + 1;
            p->spots[spot_idx].type = (CarType)type;
            p->spots[spot_idx].is_occupied = false;
            p->spots[spot_idx].current_car = NULL;
            spot_idx++;
        }
    }

    p->total_cars = params->cars_count[TYPE_REGULAR] +
                    params->cars_count[TYPE_LARGE] +
                    params->cars_count[TYPE_SPECIAL];
    p->cars = (Car *)calloc(p->total_cars, sizeof(Car));
    p->cars_generated = 0;
    p->cars_finished = 0;
    p->generator_done = false;

    queue_init(&p->inbox, p->total_cars); // во «входящих» могут одновременно оказаться все машины
    queue_init(&p->queue, params->queue_capacity);
    logger_open(&p->log, params->log_path);
    atomic_init(&p->stop, false);
    atomic_init(&p->stop_signal, 0);
}

void cleanup_parking(Parking *p) {
    if (p->spots != NULL) {
        free(p->spots);
        p->spots = NULL;
    }
    if (p->cars != NULL) {
        // семафоры созданы только у запущенных машин
        for (int i = 0; i < p->cars_generated; i++) {
            sem_destroy(&p->cars[i].granted);
        }
        free(p->cars);
        p->cars = NULL;
    }
    queue_free(&p->inbox);
    queue_free(&p->queue);
    logger_close(&p->log);
    sem_destroy(&p->dispatcher_wake);
    sem_destroy(&p->sim_end);
    pthread_mutex_destroy(&p->mx);
}

// Записываем событие в журнал с текущим тактом
void record_event(Parking *p, const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(&p->log, now_ticks(), format, args);
    va_end(args);
}

// Поиск свободного места
int find_free_spot(const Parking *p, CarType type) {
    for (int i = 0; i < p->total_spots; i++) {
        if (p->spots[i].type == type && !p->spots[i].is_occupied) {
            return i;
        }
    }
    return -1;
}
