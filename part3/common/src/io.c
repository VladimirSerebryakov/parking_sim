#include "io.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int write_all(int fd, const char *text, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, text, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        text += n;
        len -= (size_t)n;
    }
    return 0;
}

void report_error(const char *operation) {
    int saved_errno = errno;
    char text[512];
    snprintf(text, sizeof(text), "Ошибка: %s: %s\n", operation, strerror(saved_errno));
    write_all(STDERR_FILENO, text, strlen(text));
    errno = saved_errno;
}
