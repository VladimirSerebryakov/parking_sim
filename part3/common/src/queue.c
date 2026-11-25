#include "queue.h"

bool enqueue(Queue *q, int *ids, int car_idx) {
    if (q->size == q->capacity) return false;
    ids[((long long)q->head + q->size) % q->capacity] = car_idx;
    q->size++;
    return true;
}

bool dequeue(Queue *q, int *ids, int *car_idx) {
    if (q->size == 0) return false;
    *car_idx = ids[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;
    return true;
}

int queue_get(const Queue *q, const int *ids, int index) {
    return ids[((long long)q->head + index) % q->capacity];
}

void queue_remove_at(Queue *q, int *ids, int index) {
    for (int i = index; i < q->size - 1; i++) {
        ids[((long long)q->head + i) % q->capacity] = queue_get(q, ids, i + 1);
    }
    q->size--;
}
