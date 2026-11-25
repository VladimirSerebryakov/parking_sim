#ifndef UI_H
#define UI_H

#include "parking.h"

// Переключение терминала в альтернативный буфер и обратно
void ui_enter(void);
void ui_leave(void);
// Собирает кадр дашборда в новый буфер (освободить через free); len - длина текста.
// Читает общее состояние, поэтому при работающих процессах вызывать под общей блокировкой
char *build_dashboard(const Parking *p, int *len);
// Собирает и сразу выводит кадр после завершения всех дочерних процессов
void draw_dashboard(const Parking *p);
// Итоговая статистика: в терминал (с цветом) и в журнал (без цвета)
void print_final_stats(Parking *p, const char *exit_reason);

#endif // UI_H
