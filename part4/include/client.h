#ifndef PARKING_TCP_CLIENT_H
#define PARKING_TCP_CLIENT_H
#include "net.h"
#include <signal.h>
enum { ROLE_CAR = 1, ROLE_GENERATOR = 2, ROLE_MONITOR = 3 };
typedef struct {
    Connection connection; // Собственное TCP-соединение
    int id, type, tick_ms, total; // План из приветствия
    long long session; // Идентификатор экземпляра сервера
    uint32_t seq; // Последовательность команд
    long long heartbeat; // Следующее уведомление активности
    bool stopped; // Получено финальное уведомление
    bool defer_stop_ack, stop_ack_sent; // Генератор подтверждает STOP после сбора детей
    char summary[1024]; // Итоги для вывода после выхода из альтернативного буфера
} Client;
extern volatile sig_atomic_t tcp_interrupted;
int tcp_signals(void);
int text_numbers(const char *text, long long *values, size_t count);
int client_open(Client *c, const char *host, const char *port, int role, int id);
int client_send(Client *c, uint32_t type, const char *body);
int client_next(Client *c, NetFrame *f, int wait_ms); // 0 — нет, 1 — кадр, 2 — STOP, -1 — ошибка
void client_detach(Client *c);
void client_ack_stop(Client *c);
void client_close(Client *c);
void client_print(const char *text);
// Поведение автомобиля после HELLO: новый ARRIVE либо восстановление по первому STATE.
int car_drive(Client *c);
#endif
