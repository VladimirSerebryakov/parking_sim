#ifndef UNNAMED_RESOURCES_H
#define UNNAMED_RESOURCES_H

#include "shared_memory.h"
#include "fork_processes.h"

typedef struct {
    SharedHandle memory; // Общая модель и дополнительная область семафоров
    ForkContext processes; // Локальные ссылки для участников
    sem_t *storage; // Массив sem_t внутри отображения
    sem_t **semaphores; // Локальная таблица адресов объектов
    size_t ready; // Число инициализированных семафоров для отката
} UnnamedResources;

int create_unnamed_resources(UnnamedResources *r, const SimParams *params);
void destroy_unnamed_resources(UnnamedResources *r);

#endif
