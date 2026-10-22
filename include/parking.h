#ifndef PARKING_H
#define PARKING_H

#include "common.h"
#include "params.h"
#include "queue.h"
#include "logger.h"

// Общее состояние парковки
typedef struct {
    ParkingSpot *spots; // Все места подряд: сначала обычные, затем крупные, затем спец.
    int total_spots; // Общее число мест
    Queue queue; // Очередь перед въездом
    SimParams params; // Параметры (cars_count уменьшается по мере генерации авто)
    int current_tick; // Текущий такт
    int cars_served; // Обслужено
    int cars_left_queue; // Ушло из очереди по таймауту
    int cars_rejected; // Отказано (очередь заполнена)
    Logger log; // Журнал событий
} Parking;

void init_parking(Parking *p, const SimParams *params);
void cleanup_parking(Parking *p);
void record_event(Parking *p, const char *format, ...);
int find_free_spot(const Parking *p, CarType type);

#endif // PARKING_H
