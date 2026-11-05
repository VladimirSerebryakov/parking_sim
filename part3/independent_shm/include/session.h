#ifndef SESSION_H
#define SESSION_H

#include "shared_memory.h"
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#define SESSION_MAGIC UINT64_C(0x5041524b53484d01)
#define SESSION_WAIT_NS 50000000LL

typedef struct {
    int interval; // Запланированный интервал перед этим автомобилем
    pid_t pid; // PID подключённого автомобиля, 0 после отключения
    bool seen; // Этот номер уже использован
    bool finished; // Автомобиль закончил модель
} CarSlot;

typedef struct {
    uint64_t magic; // Версия протокола
    pid_t owner_pid; // Диспетчер, создавший сеанс
    pid_t generator_pid; // Подключённый генератор или 0
    int clients; // Число зарегистрированных программ
    int stop_signal; // Первый запрос остановки по сигналу
    bool started; // Первый участник запустил часы модели
    bool closing; // Новые участники больше не принимаются
    bool failed; // Ошибка участника, итоговая статистика недостоверна
} SessionMeta;

typedef struct {
    SharedHandle memory; // Локальное отображение модели
    SessionMeta *meta; // Общие метаданные сеанса
    CarSlot *slots; // Общие планы и регистрация автомобилей
    sem_t *lock, *wake, *grant; // Только необходимые этому участнику семафоры
    sem_t **owned_sems; // Таблица всех семафоров только у владельца
    bool *owned_names; // Какие имена созданы именно этим владельцем
    size_t created; // Число созданных семафоров
    int car_idx; // Номер своего слота, -1 у генератора и диспетчера
    bool attached; // Участник учтён в clients
    bool generator; // Подключение генератора
    bool owner; // Право удаления имён
    char name[96]; // Имя памяти и основа имён семафоров
} Session;

int session_name(const char *id, char *name, size_t size);
int session_create(Session *s, const char *id, const SimParams *params);
int session_connect(Session *s, const char *id, int car_idx);
int session_lock(Session *s);
int session_unlock(Session *s);
int session_wake(Session *s);
int session_disconnect(Session *s);
void session_unlink(Session *s);
void session_close(Session *s);
int session_block_signals(sigset_t *signals);
int session_signal(const sigset_t *signals);
int session_request_stop(Session *s, int sig, bool failed);
bool session_owner_alive(const Session *s);
// Доставляет решение из общего модуля диспетчера; вызывать под lock.
void session_notify(void *context, int car_idx);
int sibling_executable(const char *name, char *path, size_t size);

#endif
