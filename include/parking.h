#ifndef PARKING_H
#define PARKING_H

#include <stdbool.h>
#include <stddef.h>

#define HISTORY_SIZE 3

typedef enum {
    TYPE_REGULAR = 0, // Обычный
    TYPE_LARGE = 1, // Крупногабаритный
    TYPE_SPECIAL = 2 // Специальный (на инвалидные места)
} CarType;

typedef struct {
    int id;
    CarType type; // Тип автомобиля
    int parking_time;
    int parking_time_left; // Сколько тактов осталось стоять (если припаркован)
    int wait_time; // Сколько тактов уже ждет в очереди
    int max_wait_time; // Допустимое время ожидания
} Car;

typedef struct {
    int id;
    CarType type; // Совместимый тип
    bool is_occupied; // Занят ли
    Car current_car; // Автомобиль, занимающий место
} ParkingSpot;

// Очередь
typedef struct {
    Car *buffer; // Массив авто
    int capacity; // Вместимость
    int size; // Кол-во машин
    int head; // Индекс начала очереди
    int tail; // Индекс конца очереди
} Queue;

// Параметры
typedef struct {
    int spots_count[3]; // Количество мест каждого типа
    int cars_count[3];  // Количество машин каждого типа для генерации
    int queue_capacity;  // Вместимость очереди
    int arrival_min, arrival_max; // Диапазон интервалов прибытия
    int park_time_min, park_time_max; // Диапазон времени стоянки
    int max_wait_time; // Максимальное время в очереди
    int max_ticks; // Предельное число тактов
} SimParams;

// Диспетчер
typedef struct {
    ParkingSpot *spots;
    int total_spots;
    Queue queue;
    SimParams params;
    int current_tick;
    int cars_served;
    int cars_left_queue;
    int cars_rejected;
    char event_history[HISTORY_SIZE][128]; // история событий
    int log_fd;
} Dispatcher;

void init_dispatcher(Dispatcher *d, const SimParams *params);
void cleanup_dispatcher(Dispatcher *d);
bool is_queue_empty(const Queue *q);
bool is_queue_full(const Queue *q);
bool enqueue(Queue *q, Car car);
bool dequeue(Queue *q, Car *car);\
void record_event(Dispatcher *d, const char *format, ...);
void draw_dashboard(const Dispatcher *d);
int find_free_spot(const Dispatcher *d, CarType type);
int get_rand_range(int min, int max);

#endif // PARKING_H
