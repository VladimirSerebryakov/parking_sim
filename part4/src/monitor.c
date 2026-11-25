#include "client.h"
#include "io.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (tcp_signals() < 0) { report_error("sigaction"); return 1; }
    bool plain = argc == 4 && strcmp(argv[3], "--plain") == 0;
    if (argc != 3 && !plain) {
        client_print("Использование: parking_monitor АДРЕС ПОРТ [--plain]\n"); return 1;
    }
    Client c;
    if (client_open(&c, argv[1], argv[2], ROLE_MONITOR, 0) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение наблюдателя"); return 1;
    }
    bool terminal = isatty(STDOUT_FILENO) && !plain;
    if (terminal) client_print("\033[?1049h");
    int result = 0;
    while (!tcp_interrupted && !c.stopped) {
        NetFrame f;
        int rc = client_next(&c, &f, 50);
        if (rc < 0) { result = -1; break; }
        if (rc == 2) break;
        if (rc == 1) {
            if (f.type != NET_SNAPSHOT) { errno = EPROTO; result = -1; }
            else if (terminal) client_print(f.body);
            else {
                // Удаляем только ANSI CSI из текста сервера; снимки остаются читаемыми в файле.
                size_t out = 0;
                for (size_t i = 0; i < f.size; i++) {
                    if (f.body[i] == '\033' && i + 1 < f.size && f.body[i + 1] == '[') {
                        i += 2;
                        while (i < f.size && !(f.body[i] >= '@' && f.body[i] <= '~')) i++;
                    } else f.body[out++] = f.body[i];
                }
                write_all(STDOUT_FILENO, f.body, out);
            }
            net_frame_free(&f);
            if (result < 0) break;
        }
    }
    if (terminal) client_print("\033[?1049l");
    if (terminal && c.stopped) client_print(c.summary);
    if (c.stopped) client_print("Наблюдатель: сервер завершил моделирование\n");
    if (result < 0) report_error("связь наблюдателя с сервером");
    else client_detach(&c);
    client_close(&c);
    return result == 0 ? 0 : 1;
}
