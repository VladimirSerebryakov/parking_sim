#include "client.h"
#include "io.h"
#include <errno.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (tcp_signals() < 0) { report_error("sigaction"); return 1; }
    int id;
    if (argc != 4 || net_number(argv[3], 1, 4096, &id) < 0) {
        client_print("Использование: parking_car АДРЕС ПОРТ НОМЕР\n"); return 1;
    }
    Client c;
    if (client_open(&c, argv[1], argv[2], ROLE_CAR, id) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение автомобиля"); return 1;
    }
    char text[256]; snprintf(text, sizeof(text), "Авто #%d подключено к сеансу %lld\n", id, c.session); client_print(text);
    int result = car_drive(&c);
    if (result < 0) report_error("связь автомобиля с сервером");
    else client_detach(&c);
    client_close(&c);
    return result == 0 ? 0 : 1;
}
