#include "queue.h"
#include <stdlib.h>

// Создаём пустую очередь заданной вместимости
void queue_init(Queue *q, int capacity) {
    q->capacity = capacity;
    q->size = 0;
    q->head = 0;
    q->tail = 0;
    q->buffer = (Car **)malloc(capacity * sizeof(Car *));
}

void queue_free(Queue *q) {
    if (q->buffer != NULL) {
        free(q->buffer);
        q->buffer = NULL;
    }
}

bool is_queue_empty(const Queue *q) {
    return q->size == 0;
}

bool is_queue_full(const Queue *q) {
    return q->size == q->capacity;
}

// Добавляем в очередь
bool enqueue(Queue *q, Car *car) {
    if (is_queue_full(q)) {
        return false;
    }

    q->buffer[q->tail] = car;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    return true;
}

// Забираем из очереди
bool dequeue(Queue *q, Car **car) {
    if (is_queue_empty(q)) {
        return false;
    }
    *car = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;
    return true;
}

Car *queue_get(const Queue *q, int i) {
    return q->buffer[(q->head + i) % q->capacity];
}

void queue_remove_at(Queue *q, int i) {
    // сдвигаем все машины после i-й на одну позицию к началу
    for (int j = i; j < q->size - 1; j++) {
        q->buffer[(q->head + j) % q->capacity] = q->buffer[(q->head + j + 1) % q->capacity];
    }
    q->tail = (q->tail - 1 + q->capacity) % q->capacity;
    q->size--;
}
