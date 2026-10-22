#include "parking.h"
#include <stdlib.h>
#include <stdarg.h>

// инициализируем парковку (заполняем параметрами)
void init_parking(Parking *p, const SimParams *params) {
    p->params = *params;
    p->current_tick = 0;
    p->cars_served = 0;
    p->cars_left_queue = 0;
    p->cars_rejected = 0;
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
            spot_idx++;
        }
    }
    queue_init(&p->queue, params->queue_capacity);
    logger_open(&p->log, params->log_path);
}

void cleanup_parking(Parking *p) {
    if (p->spots != NULL) {
        free(p->spots);
        p->spots = NULL;
    }
    queue_free(&p->queue);
    logger_close(&p->log);
}

// Записываем событие в журнал с текущим тактом
void record_event(Parking *p, const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_vevent(&p->log, p->current_tick, format, args);
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
