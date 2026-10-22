#ifndef UI_H
#define UI_H

#include "parking.h"

// Переключение терминала в альтернативный буфер и обратно
void ui_enter(void);
void ui_leave(void);
// Отрисовка текущего состояния парковки
void draw_dashboard(const Parking *p);
// Итоговая статистика: в терминал (с цветом) и в журнал (без цвета)
void print_final_stats(Parking *p, const char *exit_reason);

#endif // UI_H
