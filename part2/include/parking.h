#ifndef PARKING_H
#define PARKING_H

#include "common.h"
#include "params.h"
#include "queue.h"
#include "logger.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

// Общее состояние парковки. Все поля, кроме stop, stop_signal и семафоров, читаются и меняются только под mx
typedef struct {
    pthread_mutex_t mx; // Единственный мьютекс, защищающий всё состояние
    sem_t dispatcher_wake; // «Для диспетчера есть событие»: прибытие, освобождение места или остановка
    sem_t sim_end; // «Пора завершаться»: все авто обработаны или получен сигнал (лимит времени - таймаут ожидания)
    bool end_notified; // sim_end уже поднят из-за того, что все авто обработаны
    ParkingSpot *spots; // Все места подряд: сначала обычные, затем крупные, затем спец.
    int total_spots; // Общее число мест
    Car *cars; // Все автомобили (массив выделяется заранее, живёт до конца программы)
    int total_cars; // Сколько автомобилей будет сгенерировано всего
    int cars_generated; // Сколько уже выехало на парковку (запущено потоков)
    int cars_finished; // Сколько уже завершили работу (уехали, получили отказ или отмену)
    bool generator_done; // Генератор выпустил всех, кого собирался
    Queue inbox; // Прибывшие авто, ожидающие решения диспетчера (въезд)
    Queue queue; // Очередь перед въездом (ограниченной вместимости)
    SimParams params; // Параметры
    int cars_served; // Обслужено
    int cars_left_queue; // Ушло из очереди по таймауту
    int cars_rejected; // Отказано (очередь заполнена)
    int cars_cancelled; // Ожидание прервано остановкой симуляции
    Logger log; // Журнал событий
    atomic_bool stop; // Пора останавливаться (Ctrl+C, лимит времени или все обработаны)
    atomic_int stop_signal; // Сигнал, прервавший симуляцию (SIGINT/SIGTERM), 0 - не было
} Parking;

void init_parking(Parking *p, const SimParams *params);
void cleanup_parking(Parking *p);
// Записывает событие в журнал с текущим тактом (вызывать под mx)
void record_event(Parking *p, const char *format, ...);
int find_free_spot(const Parking *p, CarType type);

#endif // PARKING_H
