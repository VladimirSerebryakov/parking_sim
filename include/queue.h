#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

// Очередь автомобилей (кольцевой буфер указателей)
typedef struct {
    Car **buffer; // Массив указателей на авто
    int capacity; // Вместимость
    int size; // Кол-во машин
    int head; // Индекс начала очереди
    int tail; // Индекс конца очереди
} Queue;

void queue_init(Queue *q, int capacity);
void queue_free(Queue *q);
bool is_queue_empty(const Queue *q);
bool is_queue_full(const Queue *q);
bool enqueue(Queue *q, Car *car);
bool dequeue(Queue *q, Car **car);
// i-я машина, считая от начала очереди (0 - первая)
Car *queue_get(const Queue *q, int i);
// Удаляет i-ю машину, сохраняя порядок остальных
void queue_remove_at(Queue *q, int i);

#endif // QUEUE_H
