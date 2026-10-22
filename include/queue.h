#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

// Очередь перед въездом (кольцевой буфер)
typedef struct {
    Car *buffer; // Массив авто
    int capacity; // Вместимость
    int size; // Кол-во машин
    int head; // Индекс начала очереди
    int tail; // Индекс конца очереди
} Queue;

void queue_init(Queue *q, int capacity);
void queue_free(Queue *q);
bool is_queue_empty(const Queue *q);
bool is_queue_full(const Queue *q);
bool enqueue(Queue *q, Car car);
bool dequeue(Queue *q, Car *car);

#endif // QUEUE_H
