#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

#define HISTORY_SIZE 3 // Сколько последних событий хранить для дашборда
#define EVENT_LEN 256 // Максимальная длина текста события (в байтах; кириллица - 2 байта на букву)

// Журнал событий
typedef struct {
    int fd; // Дескриптор файла журнала (-1, если открыть не удалось)
    char history[HISTORY_SIZE][EVENT_LEN]; // История событий, [0] - самое новое
} Logger;

void logger_open(Logger *log, const char *path);
void logger_close(Logger *log);
// Записывает событие в файл с номером такта и добавляет его в историю
void log_vevent(Logger *log, int tick, const char *format, va_list args);
// Записывает в файл готовый текст (например, итоговую статистику)
void logger_write(Logger *log, const char *text, int len);

#endif // LOGGER_H
