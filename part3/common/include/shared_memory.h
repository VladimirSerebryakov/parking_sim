#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include "parking.h"
#include <stddef.h>

// Заголовок общей модели. Примитивы синхронизации сюда не входят.
typedef struct {
    ParkingState state; // Данные парковки
    size_t spots_offset, cars_offset; // Смещения массивов
    size_t inbox_offset, queue_offset; // Смещения очередей
    size_t extra_offset; // Выровненная дополнительная область для варианта реализации
} SharedMemory;

typedef struct {
    SharedMemory *shared; // Локальный адрес отображения
    Parking parking; // Локальные адреса массивов и дескриптор журнала
    size_t size; // Размер отображения
    char name[96]; // Имя объекта: раннее удаление у fork, сохранение у независимого сеанса
    bool name_owned; // Созданное нами имя ещё существует и требует unlink
} SharedHandle;

// extra_bytes используются неименованными семафорами; у fork_named равны нулю.
int create_shared_memory(SharedHandle *h, const SimParams *params,
                         const char *variant, size_t extra_bytes);
// Сеанс независимых программ сохраняет имя до закрытия регистрации участников.
int create_session_memory(SharedHandle *h, const SimParams *params, const char *name, size_t extra_bytes);
int attach_shared_memory(SharedHandle *h, const char *name);
int unlink_shared_memory(SharedHandle *h);
void destroy_shared_memory(SharedHandle *h);

#endif
