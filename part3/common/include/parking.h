#ifndef PARKING_H
#define PARKING_H

#include "common.h"
#include "params.h"
#include "queue.h"
#include "sim_clock.h"

#define HISTORY_SIZE 3
#define EVENT_LEN 320

// Только данные: никаких указателей, дескрипторов или примитивов конкретной реализации.
typedef struct {
    SimParams params; // Параметры запуска
    SimClock clock; // Единые часы всех участников
    Queue inbox; // Входящие автомобили
    Queue queue; // Очередь ожидания
    int total_spots, total_cars; // Размеры массивов
    int cars_generated, cars_finished; // Число запущенных и завершившихся автомобилей
    int cars_served, cars_left_queue, cars_rejected, cars_cancelled; // Статистика
    bool generator_done, end_notified; // Генерация закончена, завершение уже объявлено
    bool stop; // Остановка, читается и меняется под общей блокировкой
    char history[HISTORY_SIZE][EVENT_LEN]; // Последние события
} ParkingState;

// Локальное представление: адреса вычислены после mmap; fd унаследован или открыт участником.
typedef struct {
    ParkingState *state; // Общие данные
    ParkingSpot *spots; // Общий массив мест
    Car *cars; // Общий массив автомобилей
    int *inbox_ids, *queue_ids; // Общие массивы индексов очередей
    int log_fd; // Локальный дескриптор журнала
} Parking;

// Вызываются под общей блокировкой либо после завершения детей.
void record_event(Parking *p, const char *format, ...);
int find_free_spot(const Parking *p, CarType type);

#endif
