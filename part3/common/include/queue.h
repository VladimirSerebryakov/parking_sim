#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>

// Метаданные кольцевой очереди; сам массив индексов находится отдельно в общей памяти.
typedef struct {
    int capacity; // Вместимость
    int size; // Число автомобилей
    int head; // Начало очереди
} Queue;

bool enqueue(Queue *q, int *ids, int car_idx);
bool dequeue(Queue *q, int *ids, int *car_idx);
int queue_get(const Queue *q, const int *ids, int index);
void queue_remove_at(Queue *q, int *ids, int index);

#endif
