#include "parking.h"
#include "io.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const type_names[3] = {"Обычный", "Крупногабаритный", "Специальный"};

void record_event(Parking *p, const char *format, ...) {
    char event[EVENT_LEN];
    va_list args;
    va_start(args, format);
    vsnprintf(event, sizeof(event), format, args);
    va_end(args);
    char line[EVENT_LEN + 64];
    snprintf(line, sizeof(line), "[Такт %lld] %s\n", now_ticks(&p->state->clock), event);
    if (p->log_fd >= 0) write_all(p->log_fd, line, strlen(line));
    for (int i = HISTORY_SIZE - 1; i > 0; i--)
        memcpy(p->state->history[i], p->state->history[i - 1], EVENT_LEN);
    memcpy(p->state->history[0], event, strlen(event) + 1);
}

int find_free_spot(const Parking *p, CarType type) {
    for (int i = 0; i < p->state->total_spots; i++) {
        if (p->spots[i].type == type && p->spots[i].car_idx == -1) return i;
    }
    return -1;
}
