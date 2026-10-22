#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

void logger_open(Logger *log, const char *path) {
    memset(log->history, 0, sizeof(log->history));
    log->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log->fd == -1) {
        char msg[512];
        int len = snprintf(msg, sizeof(msg), "Предупреждение: не удалось открыть журнал '%s', события не будут сохранены\n", path);
        write(STDERR_FILENO, msg, len);
    }
}

void logger_close(Logger *log) {
    if (log->fd != -1) {
        close(log->fd);
        log->fd = -1;
    }
}

void log_vevent(Logger *log, int tick, const char *format, va_list args) {
    char buffer[EVENT_LEN];
    vsnprintf(buffer, sizeof(buffer), format, args);

    if (log->fd != -1) {
        char log_buffer[160];
        int log_len = snprintf(log_buffer, sizeof(log_buffer), "[Такт %d] %s\n", tick, buffer);
        write(log->fd, log_buffer, log_len);
    }

    for (int i = HISTORY_SIZE - 1; i > 0; i--) {
        strncpy(log->history[i], log->history[i - 1], EVENT_LEN);
    }
    strncpy(log->history[0], buffer, EVENT_LEN);
}

void logger_write(Logger *log, const char *text, int len) {
    if (log->fd != -1) {
        write(log->fd, text, len);
    }
}
