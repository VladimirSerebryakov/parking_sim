#ifndef MQ_PROTOCOL_H
#define MQ_PROTOCOL_H
#include "parking.h"
#include "process_utils.h"
#include <mqueue.h>
#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h>

#define MQ_MAGIC UINT64_C(0x5041524b4d510001)
#define MQ_CAPACITY 10
typedef enum {
    MQ_HELLO, MQ_WELCOME, MQ_ARRIVE, MQ_STATUS, MQ_START, MQ_LEAVE, MQ_BYE,
    MQ_ACK, MQ_STOP_REQUEST, MQ_STOP, MQ_FAIL, MQ_ERROR
} MessageType;
typedef struct {
    uint64_t magic, cookie; // Версия и идентификатор подключения
    pid_t pid, owner_pid; // Адресат ответа/отправитель запроса и диспетчер
    unsigned int seq; // Номер запроса, 0 у уведомления
    int id; // 0 — генератор, 1..N — автомобиль
    MessageType type; // Команда протокола
    int error, signal; // Код ошибки или сигнал остановки
    SimClock clock; // Часы модели из приветствия
    Car car; // План или текущее состояние авто
    SimParams params; // Параметры в приветствии
} Message;

typedef struct {
    mqd_t requests, replies; // Две POSIX-очереди с O_NONBLOCK
    sem_t *sems[4]; // Готовность, блокировка ответов, события запросов и ответов
    bool owner, queues_owned[2], sems_owned[4]; // Только собственные имена подлежат удалению
    char base[96]; // /parking_mq_СЕАНС
} Transport;

typedef struct {
    Transport transport; // Локальные дескрипторы
    pid_t pid, owner_pid; // Участник и сервер
    uint64_t cookie; // Уникальное подключение
    unsigned int seq; // Последовательность запросов
    int id; // Роль/номер автомобиля
    bool stop, has_status; // Полученные уведомления
    CarStatus status; // Последнее состояние
    Message welcome, last; // Приветствие и последнее принятое сообщение
} MqClient;

int transport_name(const char *id, char *base, size_t size);
int transport_create(Transport *t, const char *id);
int transport_connect(Transport *t, const char *id);
int transport_publish(Transport *t);
void transport_close(Transport *t);
int transport_lock(Transport *t);
int transport_unlock(Transport *t);
int transport_send(mqd_t queue, const Message *message);
int transport_receive(mqd_t queue, Message *message);
bool transport_alive(const Transport *t);
int client_connect(MqClient *c, const char *session, int id);
int client_poll(MqClient *c);
int client_call(MqClient *c, MessageType type, int sig, Message *reply);
int client_disconnect(MqClient *c);
void client_close(MqClient *c);
#endif
