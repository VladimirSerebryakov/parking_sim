#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <time.h>

typedef enum { TYPE_REGULAR, TYPE_LARGE, TYPE_SPECIAL } CarType;
typedef enum { STRATEGY_FIFO, STRATEGY_LIFO, STRATEGY_RANDOM, STRATEGY_SHORT } Strategy;
typedef enum {
    CAR_ARRIVING, CAR_QUEUED, CAR_PARKED, CAR_REJECTED,
    CAR_TIMEOUT, CAR_CANCELLED, CAR_DONE
} CarStatus;

// Общие данные без адресов памяти и идентификаторов потоков.
typedef struct {
    int id; // Номер автомобиля
    CarType type; // Тип автомобиля
    int parking_time; // Длительность стоянки в тактах
    long long arrival_tick; // Такт прибытия
    long long leave_tick; // Планируемый такт отъезда
    CarStatus status; // Состояние под общей блокировкой
    int spot_idx; // Индекс места или -1
    struct timespec deadline; // Абсолютный срок ожидания, CLOCK_REALTIME
} Car;

typedef struct {
    int id; // Номер места
    CarType type; // Совместимый тип автомобиля
    int car_idx; // Индекс автомобиля, -1 означает свободное место
} ParkingSpot;

extern const char *const type_names[3];

#endif
