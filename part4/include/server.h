#ifndef PARKING_TCP_SERVER_H
#define PARKING_TCP_SERVER_H
#include "client.h"
#include "model.h"

#define MAX_PEERS 1024
typedef struct {
    Connection connection; // Сокет и ограниченные буферы
    int role, id, last_status; // Роль, номер авто и доставленный статус
    uint32_t seq; // Последняя команда соединения
    bool closing, stop_sent; // Закрытие после отправки / ожидается ACK остановки
    long long snapshot_at; // Следующая отправка табло
} Peer;
typedef struct {
    Model model; // Единственная копия состояния парковки
    Peer *peers; // Ограниченная таблица соединений
    int listener; // Принимающий сокет
    long long session, stopping_at; // Экземпляр сервера и срок остановки
    char summary[1024]; // Финальная причина и статистика
} Server;

void server_drop(Peer *p);
void server_state(Server *s, Peer *p, uint32_t seq);
void server_snapshot(Server *s, Peer *p);
void server_request(Server *s, Peer *p, const NetFrame *f);
void server_stop(Server *s, const char *reason);
#endif
