#ifndef PARKING_NET_H
#define PARKING_NET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MAGIC UINT32_C(0x504b3401)
#define NET_HEADER 16U
#define NET_MAX_BODY (1024U * 1024U)
#define NET_MAX_OUTPUT (2U * NET_MAX_BODY)
#define NET_TIMEOUT_NS 5000000000LL
typedef enum {
    NET_HELLO = 1, NET_WELCOME, NET_ARRIVE, NET_STATE, NET_LEAVE,
    NET_PLAN, NET_PLAN_REPLY, NET_SNAPSHOT, NET_STOP, NET_ACK,
    NET_DETACH, NET_ERROR, NET_PING, NET_PONG
} NetType;
typedef struct {
    uint32_t type, seq; // Тип и номер запроса; 0 у серверного уведомления
    size_t size; // Размер полезных данных без завершающего нуля
    char *body; // Локальный буфер декодированного сообщения
} NetFrame;
typedef struct {
    int fd; // Неблокирующий сокет, -1 после закрытия
    unsigned char *input, *output; // Ограниченные локальные буферы
    size_t input_size, input_capacity, output_size, output_offset;
    size_t max_body; // Сервер принимает короткие команды, клиент — также снимки
    long long last_input, queued_at; // Сроки активности и отправки
} Connection;

void net_init(Connection *c, int fd, size_t max_body);
void net_close(Connection *c);
int net_read(Connection *c); // 0 — данные/EAGAIN, -1 — ошибка или EOF
int net_next(Connection *c, NetFrame *frame); // 1 — сообщение, 0 — неполное, -1 — ошибка
void net_frame_free(NetFrame *frame);
int net_queue(Connection *c, uint32_t type, uint32_t seq, const char *body);
int net_flush(Connection *c);
bool net_pending(const Connection *c);
int net_listen(const char *host, const char *port);
int net_connect(const char *host, const char *port, int timeout_ms);
int net_accept(int listener);
int net_number(const char *text, int low, int high, int *value);
#endif
