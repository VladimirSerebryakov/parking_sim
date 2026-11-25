#ifndef IO_H
#define IO_H

#include <stddef.h>

// Вывод через дескриптор с обработкой EINTR и частичной записи.
int write_all(int fd, const char *text, size_t len);
void report_error(const char *operation);

#endif
