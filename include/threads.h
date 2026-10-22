#ifndef THREADS_H
#define THREADS_H

#include "parking.h"

// Блокирует SIGINT/SIGTERM/SIGUSR1 в вызывающем потоке. Вызывать первым делом в main:
// созданные позже потоки унаследуют маску, а сигналы будет принимать только поток сигналов
void block_signals(void);
// Запускает потоки: сигналов, диспетчера, генератора (он сам запускает потоки автомобилей) и отрисовщика
void threads_start(Parking *p);
// Будит диспетчера и ждёт завершения всех потоков. Перед вызовом должен быть выставлен p->stop
void threads_join(Parking *p);

#endif // THREADS_H
