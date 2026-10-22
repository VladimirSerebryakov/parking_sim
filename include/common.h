#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>

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
    STRATEGY_DEADLINE = 2, // Тот, у кого раньше всех истекает время ожидания
    STRATEGY_SHORT = 3 // С самой короткой стоянкой
} Strategy;

// Автомобиль
typedef struct {
    int id;
    CarType type; // Тип автомобиля
    int parking_time;
    int parking_time_left; // Сколько тактов осталось стоять (если припаркован)
    int wait_time; // Сколько тактов уже ждет в очереди
    int max_wait_time; // Допустимое время ожидания
} Car;

// Парковочное место
typedef struct {
    int id;
    CarType type; // Совместимый тип
    bool is_occupied; // Занят ли
    Car current_car; // Автомобиль, занимающий место
} ParkingSpot;

#endif // COMMON_H
