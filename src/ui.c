#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

void ui_enter(void) {
    write(STDOUT_FILENO, "\033[?1049h", 8);
}

void ui_leave(void) {
    write(STDOUT_FILENO, "\033[?1049l", 8);
}

// Отрисовка
void draw_dashboard(const Parking *p) {
    // размер буфера зависит от числа мест и вместимости очереди, иначе при большом числе мест буфер переполняется
    size_t screen_size = 1024 + p->total_spots * 128 + p->queue.capacity * 16 + HISTORY_SIZE * 160;
    char *screen = (char *)malloc(screen_size);
    if (screen == NULL) return;
    int offset = 0;

    // Очищаем экран полностью и ставим курсор в левый верхний угол
    offset += snprintf(screen + offset, screen_size - offset, "\033[2J\033[H");

    offset += snprintf(screen + offset, screen_size - offset,
        "=====================================================\n"
        " \033[1;36m🕒 АВТОМАТИЗИРОВАННАЯ ПАРКОВКА | ТАКТ: %2d\033[0m\n"
        " Стратегия: %s | Seed: %u | Такт: %d мс\n"
        "=====================================================\n"
        " \033[1;33m📊 СТАТИСТИКА:\033[0m Обслужено: \033[32m%d\033[0m | Ушло (таймаут): \033[31m%d\033[0m | Отказ: \033[31m%d\033[0m\n"
        "=====================================================\n"
        " \033[1;34m[P] СОСТОЯНИЕ ПАРКОВКИ:\033[0m\n",
        p->current_tick, strategy_name(p->params.strategy), p->params.seed, p->params.tick_ms,
        p->cars_served, p->cars_left_queue, p->cars_rejected);

    const char *type_tags[] = {"ОБЫЧ", "КРУП", "СПЕЦ"};
    for (int i = 0; i < p->total_spots; i++) {
        if (p->spots[i].is_occupied) {
            offset += snprintf(screen + offset, screen_size - offset,
                " [%s %d] \033[31m[  ЗАНЯТО  ]\033[0m Авто #%02d (Осталось: %2d т.)\n",
                type_tags[p->spots[i].type], p->spots[i].id,
                p->spots[i].current_car.id, p->spots[i].current_car.parking_time_left);
        } else {
            offset += snprintf(screen + offset, screen_size - offset,
                " [%s %d] \033[32m[ СВОБОДНО ]\033[0m\n",
                type_tags[p->spots[i].type], p->spots[i].id);
        }
    }

    offset += snprintf(screen + offset, screen_size - offset,
        "\n \033[1;35m🚗 ОЧЕРЕДЬ (%d/%d):\033[0m ", p->queue.size, p->queue.capacity);
    for (int i = 0; i < p->queue.capacity; i++) {
        if (i < p->queue.size) {
            int idx = (p->queue.head + i) % p->queue.capacity;
            offset += snprintf(screen + offset, screen_size - offset, "[ #%02d ] ", p->queue.buffer[idx].id);
        } else {
            offset += snprintf(screen + offset, screen_size - offset, "[    ] ");
        }
    }

    offset += snprintf(screen + offset, screen_size - offset,
        "\n\n=====================================================\n"
        " \033[1;37m📝 ПОСЛЕДНИЕ СОБЫТИЯ:\033[0m\n");
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (strlen(p->log.history[i]) > 0) {
            if (i == 0) offset += snprintf(screen + offset, screen_size - offset, "\033[1m-> %s\033[0m\n", p->log.history[i]);
            else offset += snprintf(screen + offset, screen_size - offset, "   %s\n", p->log.history[i]);
        }
    }
    offset += snprintf(screen + offset, screen_size - offset, "=====================================================\n");

    write(STDOUT_FILENO, screen, offset);
    free(screen);
}

// Формирует текст итоговой статистики; colored - добавлять ли ANSI-цвета
static int format_final_stats(char *buf, size_t size, const Parking *p, const char *exit_reason, bool colored) {
    const char *green = colored ? "\033[32m" : "";
    const char *red = colored ? "\033[31m" : "";
    const char *reset = colored ? "\033[0m" : "";
    return snprintf(buf, size,
        "\n========================================\n"
        " %s\n"
        "========================================\n"
        " ИТОГОВАЯ СТАТИСТИКА:\n"
        " - Обслужено автомобилей: %s%d%s\n"
        " - Ушло по таймауту из очереди: %s%d%s\n"
        " - Отказано (очередь заполнена): %s%d%s\n"
        " - Стратегия: %s, seed: %u (повтор: -R %u)\n"
        "========================================\n",
        exit_reason,
        green, p->cars_served, reset,
        red, p->cars_left_queue, reset,
        red, p->cars_rejected, reset,
        strategy_name(p->params.strategy), p->params.seed, p->params.seed);
}

void print_final_stats(Parking *p, const char *exit_reason) {
    char stats[1024];

    int len = format_final_stats(stats, sizeof(stats), p, exit_reason, true);
    write(STDOUT_FILENO, stats, len);
    write(STDOUT_FILENO, "\n", 1);

    len = format_final_stats(stats, sizeof(stats), p, exit_reason, false);
    logger_write(&p->log, stats, len);
}
