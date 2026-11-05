#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>

// Тип автомобиля и совместимого с ним парковочного места
typedef enum {
    TYPE_REGULAR = 0, // Обычный
    TYPE_LARGE = 1, // Крупногабаритный
    TYPE_SPECIAL = 2 // Специальный (на инвалидные места)
} CarType;

// Стратегия выбора автомобиля из очереди при освобождении места
typedef enum {
    STRATEGY_FIFO = 0, // Дольше всех ждущий
    STRATEGY_LIFO = 1, // Последний прибывший
    STRATEGY_RANDOM = 2, // Случайный из подходящих
    STRATEGY_SHORT = 3 // С самой короткой стоянкой
} Strategy;

// Состояние автомобиля
typedef enum {
    CAR_ARRIVING, // Приехал и ждёт решения диспетчера
    CAR_QUEUED, // Стоит в очереди перед въездом
    CAR_PARKED, // Диспетчер назначил место
    CAR_REJECTED, // Очередь была заполнена - уехал
    CAR_TIMEOUT, // Не дождался места - ушёл из очереди
    CAR_CANCELLED, // Симуляция остановлена, пока авто ждало
    CAR_DONE // Отстоял и уехал
} CarStatus;

// Автомобиль (у каждого свой поток)
typedef struct {
    int id;
    CarType type; // Тип автомобиля
    int parking_time; // Сколько тактов стоять
    int arrival_tick; // Такт прибытия
    int leave_tick; // Такт, в который авто уедет (если припарковано)
    int max_wait_time; // Допустимое время ожидания
    CarStatus status; // Текущее состояние (меняется под мьютексом парковки)
    int spot_idx; // Индекс назначенного места (если PARKED)
    sem_t granted; // Диспетчер сообщает через него о своём решении
    pthread_t thread; // Поток автомобиля
} Car;

// Парковочное место
typedef struct {
    int id;
    CarType type; // Совместимый тип
    bool is_occupied; // Занят ли
    Car *current_car; // Автомобиль, занимающий место (NULL, если свободно)
} ParkingSpot;

#endif // COMMON_H
