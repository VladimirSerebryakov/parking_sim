#ifndef FORK_SYNC_H
#define FORK_SYNC_H

#include "parking.h"
#include <semaphore.h>

// Только локальные указатели: поведение процессов не зависит от способа создания семафоров.
typedef struct {
    Parking parking; // Представление общей модели
    sem_t *state_lock; // Общая блокировка
    sem_t *dispatcher_wake; // Уведомление диспетчера
    sem_t *sim_end; // Завершение обработки автомобилей
    sem_t **granted; // Локальная таблица персональных семафоров
    bool named_semaphores; // Именованные дескрипторы закрываются в каждом ребёнке
} ForkContext;

void init_fork_context(ForkContext *ctx, const Parking *parking, sem_t **semaphores, bool named);
void sem_wait_retry(sem_t *sem);
void sem_post_checked(sem_t *sem);
void state_lock(ForkContext *ctx);
void state_unlock(ForkContext *ctx);
int close_child_semaphores(ForkContext *ctx);

#endif
