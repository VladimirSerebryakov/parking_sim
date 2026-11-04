#ifndef NAMED_RESOURCES_H
#define NAMED_RESOURCES_H

#include "shared_memory.h"
#include "fork_processes.h"

typedef struct {
    SharedHandle memory; // Общая память содержит только модель, без sem_t
    ForkContext processes; // Локальные ссылки для участников
    sem_t **semaphores; // Дескрипторы, возвращённые sem_open
    size_t opened; // Число успешно открытых семафоров для отката
    char pending_name[128]; // Только собственное имя, которое ещё нужно удалить
} NamedResources;

int create_named_resources(NamedResources *r, const SimParams *params);
void destroy_named_resources(NamedResources *r);

#endif
