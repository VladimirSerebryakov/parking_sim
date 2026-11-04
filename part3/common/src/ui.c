#include "ui.h"
#include "sim_clock.h"
#include "io.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

void ui_enter(void) {
    write_all(STDOUT_FILENO, "\033[?1049h", 8);
}

void ui_leave(void) {
    write_all(STDOUT_FILENO, "\033[?1049l", 8);
}

// Сборка кадра дашборда
char *build_dashboard(const Parking *p, int *len) {
    // размер буфера зависит от числа мест и вместимости очереди, иначе при большом числе мест буфер переполняется
    size_t screen_size = 1024 + (size_t)p->state->total_spots * 192 + (size_t)p->state->queue.capacity * 24 + HISTORY_SIZE * (EVENT_LEN + 16);
    if (screen_size > INT_MAX) return NULL;
    char *screen = (char *)malloc(screen_size);
    if (screen == NULL) return NULL;
    int offset = 0;
    long long now = now_ticks(&p->state->clock);

    // Очищаем экран полностью и ставим курсор в левый верхний угол
    offset += snprintf(screen + offset, screen_size - offset, "\033[2J\033[H");

    offset += snprintf(screen + offset, screen_size - offset,
        "=====================================================\n"
        " \033[1;36m🕒 АВТОМАТИЗИРОВАННАЯ ПАРКОВКА | ТАКТ: %2lld\033[0m\n"
        " Стратегия: %s | Seed: %u | Такт: %d мс\n"
        "=====================================================\n"
        " \033[1;33m📊 СТАТИСТИКА:\033[0m Обслужено: \033[32m%d\033[0m | Ушло (таймаут): \033[31m%d\033[0m | Отказ: \033[31m%d\033[0m\n"
        "=====================================================\n"
        " \033[1;34m[P] СОСТОЯНИЕ ПАРКОВКИ:\033[0m\n",
        now, strategy_name(p->state->params.strategy), p->state->params.seed, p->state->params.tick_ms,
        p->state->cars_served, p->state->cars_left_queue, p->state->cars_rejected);

    const char *type_tags[] = {"ОБЫЧ", "КРУП", "СПЕЦ"};
    for (int i = 0; i < p->state->total_spots; i++) {
        if (p->spots[i].car_idx != -1) {
            const Car *car = &p->cars[p->spots[i].car_idx];
            long long time_left = car->leave_tick - now;
            if (time_left < 0) time_left = 0;
            offset += snprintf(screen + offset, screen_size - offset,
                " [%s %d] \033[31m[  ЗАНЯТО  ]\033[0m Авто #%02d (Осталось: %2lld т.)\n",
                type_tags[p->spots[i].type], p->spots[i].id, car->id, time_left);
        } else {
            offset += snprintf(screen + offset, screen_size - offset,
                " [%s %d] \033[32m[ СВОБОДНО ]\033[0m\n",
                type_tags[p->spots[i].type], p->spots[i].id);
        }
    }

    offset += snprintf(screen + offset, screen_size - offset,
        "\n \033[1;35m🚗 ОЧЕРЕДЬ (%d/%d):\033[0m ", p->state->queue.size, p->state->queue.capacity);
    for (int i = 0; i < p->state->queue.capacity; i++) {
        if (i < p->state->queue.size) {
            offset += snprintf(screen + offset, screen_size - offset, "[ #%02d ] ", p->cars[queue_get(&p->state->queue, p->queue_ids, i)].id);
        } else {
            offset += snprintf(screen + offset, screen_size - offset, "[    ] ");
        }
    }

    offset += snprintf(screen + offset, screen_size - offset,
        "\n\n=====================================================\n"
        " \033[1;37m📝 ПОСЛЕДНИЕ СОБЫТИЯ:\033[0m\n");
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (strlen(p->state->history[i]) > 0) {
            if (i == 0) offset += snprintf(screen + offset, screen_size - offset, "\033[1m-> %s\033[0m\n", p->state->history[i]);
            else offset += snprintf(screen + offset, screen_size - offset, "   %s\n", p->state->history[i]);
        }
    }
    offset += snprintf(screen + offset, screen_size - offset, "=====================================================\n");

    *len = offset;
    return screen;
}

// Отрисовка
void draw_dashboard(const Parking *p) {
    int len;
    char *screen = build_dashboard(p, &len);
    if (screen == NULL) return;
    write_all(STDOUT_FILENO, screen, len);
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
        " - Ожидание прервано остановкой: %d\n"
        " - Стратегия: %s, seed: %u (повтор: -R %u)\n"
        "========================================\n",
        exit_reason,
        green, p->state->cars_served, reset,
        red, p->state->cars_left_queue, reset,
        red, p->state->cars_rejected, reset,
        p->state->cars_cancelled,
        strategy_name(p->state->params.strategy), p->state->params.seed, p->state->params.seed);
}

void print_final_stats(Parking *p, const char *exit_reason) {
    char stats[1024];

    int len = format_final_stats(stats, sizeof(stats), p, exit_reason, true);
    write_all(STDOUT_FILENO, stats, len);
    write_all(STDOUT_FILENO, "\n", 1);

    len = format_final_stats(stats, sizeof(stats), p, exit_reason, false);
    if (p->log_fd >= 0) write_all(p->log_fd, stats, (size_t)len);
}
