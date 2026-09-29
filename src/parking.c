#include "parking.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

// инициализируем диспетчер (заполняем параметрами)
void init_dispatcher(Dispatcher *d, const SimParams *params) {
    d->params = *params;
    d->current_tick = 0;
    d->cars_served = 0;
    d->cars_left_queue = 0;
    d->cars_rejected = 0;
    d->total_spots = params->spots_count[TYPE_REGULAR] +
                     params->spots_count[TYPE_LARGE] +
                     params->spots_count[TYPE_SPECIAL];
    d->spots = (ParkingSpot *)malloc(d->total_spots * sizeof(ParkingSpot));

    int spot_idx = 0;
    for (int type = 0; type < 3; type++) {
        for (int i = 0; i < params->spots_count[type]; i++) {
            d->spots[spot_idx].id = spot_idx + 1;
            d->spots[spot_idx].type = (CarType)type;
            d->spots[spot_idx].is_occupied = false;
            spot_idx++;
        }
    }
    d->queue.capacity = params->queue_capacity;
    d->queue.size = 0;
    d->queue.head = 0;
    d->queue.tail = 0;
    d->queue.buffer = (Car*)malloc(params->queue_capacity * sizeof(Car));
    d->log_fd = open("../parking_result.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void cleanup_dispatcher(Dispatcher *d) {
    if (d->spots != NULL) {
        free(d->spots);
        d->spots = NULL;
    }
    if (d->queue.buffer != NULL) {
        free(d->queue.buffer);
        d->queue.buffer = NULL;
    }
    if (d->log_fd != -1) {
        close(d->log_fd);
        d->log_fd = -1;
    }
}

bool is_queue_empty(const Queue *q) {
    return q->size == 0;
}

bool is_queue_full(const Queue *q) {
    return q->size == q->capacity;
}

// Добавляем в очередь
bool enqueue(Queue *q, Car car) {
    if (is_queue_full(q)) {
        return false;
    }

    q->buffer[q->tail] = car;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    return true;
}

// Забираем из очереди
bool dequeue(Queue *q, Car *car) {
    if (is_queue_empty(q)) {
        return false;
    }
    *car = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;
    return true;
}

void record_event(Dispatcher *d, const char *format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (d->log_fd != -1) {
        char log_buffer[160];
        int log_len = snprintf(log_buffer, sizeof(log_buffer), "[Такт %d] %s\n", d->current_tick, buffer);
        write(d->log_fd, log_buffer, log_len);
    }

    for (int i = HISTORY_SIZE - 1; i > 0; i--) {
        strncpy(d->event_history[i], d->event_history[i - 1], 128);
    }
    strncpy(d->event_history[0], buffer, 128);
}

// Отрисовка
void draw_dashboard(const Dispatcher *d) {
    char screen[2048] = {0};
    int offset = 0;

    // Очищаем экран полностью и ставим курсор в левый верхний угол
    offset += snprintf(screen + offset, sizeof(screen) - offset, "\033[2J\033[H");

    offset += snprintf(screen + offset, sizeof(screen) - offset,
        "=====================================================\n"
        " \033[1;36m🕒 АВТОМАТИЗИРОВАННАЯ ПАРКОВКА | ТАКТ: %2d\033[0m\n"
        "=====================================================\n"
        " \033[1;33m📊 СТАТИСТИКА:\033[0m Обслужено: \033[32m%d\033[0m | Ушло (таймаут): \033[31m%d\033[0m | Отказ: \033[31m%d\033[0m\n"
        "=====================================================\n"
        " \033[1;34m[P] СОСТОЯНИЕ ПАРКОВКИ:\033[0m\n",
        d->current_tick, d->cars_served, d->cars_left_queue, d->cars_rejected);

    const char *type_tags[] = {"ОБЫЧ", "КРУП", "СПЕЦ"};
    for (int i = 0; i < d->total_spots; i++) {
        if (d->spots[i].is_occupied) {
            offset += snprintf(screen + offset, sizeof(screen) - offset,
                " [%s %d] \033[31m[  ЗАНЯТО  ]\033[0m Авто #%02d (Осталось: %2d т.)\n",
                type_tags[d->spots[i].type], d->spots[i].id,
                d->spots[i].current_car.id, d->spots[i].current_car.parking_time_left);
        } else {
            offset += snprintf(screen + offset, sizeof(screen) - offset,
                " [%s %d] \033[32m[ СВОБОДНО ]\033[0m\n",
                type_tags[d->spots[i].type], d->spots[i].id);
        }
    }

    offset += snprintf(screen + offset, sizeof(screen) - offset,
        "\n \033[1;35m🚗 ОЧЕРЕДЬ (%d/%d):\033[0m ", d->queue.size, d->queue.capacity);
    for (int i = 0; i < d->queue.capacity; i++) {
        if (i < d->queue.size) {
            int idx = (d->queue.head + i) % d->queue.capacity;
            offset += snprintf(screen + offset, sizeof(screen) - offset, "[ #%02d ] ", d->queue.buffer[idx].id);
        } else {
            offset += snprintf(screen + offset, sizeof(screen) - offset, "[    ] ");
        }
    }

    offset += snprintf(screen + offset, sizeof(screen) - offset,
        "\n\n=====================================================\n"
        " \033[1;37m📝 ПОСЛЕДНИЕ СОБЫТИЯ:\033[0m\n");
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (strlen(d->event_history[i]) > 0) {
            if (i == 0) offset += snprintf(screen + offset, sizeof(screen) - offset, "\033[1m-> %s\033[0m\n", d->event_history[i]);
            else offset += snprintf(screen + offset, sizeof(screen) - offset, "   %s\n", d->event_history[i]);
        }
    }
    offset += snprintf(screen + offset, sizeof(screen) - offset, "=====================================================\n");

    write(STDOUT_FILENO, screen, offset);
}

// Поиск свободного места
int find_free_spot(const Dispatcher *d, CarType type) {
    for (int i = 0; i < d->total_spots; i++) {
        if (d->spots[i].type == type && !d->spots[i].is_occupied) {
            return i;
        }
    }
    return -1;
}

// Генерация рандомного числа
int get_rand_range(int min, int max) {
    if (min == max) return min;
    return min + rand() % (max - min + 1);
}
