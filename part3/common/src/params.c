#include "params.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include "io.h"

// Имена стратегий в аргументе -g и в выводе (индекс = значение Strategy)
static const char *strategy_args[] = {"fifo", "lifo", "random", "short"};
static const char *strategy_names[] = {"FIFO", "LIFO", "RANDOM", "SHORT"};

const char *strategy_name(Strategy s) {
    return strategy_names[s];
}

// Ищем стратегию по имени из -g; -1, если такой нет
static int parse_strategy(const char *arg) {
    for (int i = 0; i < 4; i++) {
        if (strcmp(arg, strategy_args[i]) == 0) return i;
    }
    return -1;
}

int parse_params(int argc, char *argv[], SimParams *params) {
    // значения по умолчанию
    *params = (SimParams){
        .spots_count = {2, 1, 1},
        .cars_count = {15, 12, 10},
        .queue_capacity = 3,
        .arrival_min = 0,
        .arrival_max = 2,
        .park_time_min = 7,
        .park_time_max = 10,
        .max_wait_time = 8,
        .max_ticks = 100,
        .tick_ms = 800,
        .seed = (unsigned int)time(NULL),
        .strategy = STRATEGY_FIFO,
        .log_path = DEFAULT_LOG_PATH
    };

    int opt;
    opterr = 0;
    optind = 1;
    while ((opt = getopt(argc, argv, "t:q:r:l:s:w:a:b:p:P:c:C:S:d:R:g:L:")) != -1) {
        if (strchr("tqrlswabpPcCSdR", opt) != NULL && opt != '?') {
            char *end;
            errno = 0;
            long long value = strtoll(optarg, &end, 10);
            long long upper = opt == 'R' ? (long long)UINT_MAX : INT_MAX;
            if (errno || *optarg == '\0' || *end != '\0' || value < 0 || value > upper) {
                const char err[] = "Ошибка: ожидается неотрицательное целое число в допустимом диапазоне\n";
                write_all(STDERR_FILENO, err, sizeof(err) - 1);
                return -1;
            }
        }
        switch (opt) {
            case 't': params->max_ticks = atoi(optarg); break;
            case 'q': params->queue_capacity = atoi(optarg); break;
            case 'r': params->spots_count[TYPE_REGULAR] = atoi(optarg); break;
            case 'l': params->spots_count[TYPE_LARGE] = atoi(optarg); break;
            case 's': params->spots_count[TYPE_SPECIAL] = atoi(optarg); break;
            case 'w': params->max_wait_time = atoi(optarg); break;
            case 'a': params->arrival_min = atoi(optarg); break;
            case 'b': params->arrival_max = atoi(optarg); break;
            case 'p': params->park_time_min = atoi(optarg); break;
            case 'P': params->park_time_max = atoi(optarg); break;
            case 'c': params->cars_count[TYPE_REGULAR] = atoi(optarg); break;
            case 'C': params->cars_count[TYPE_LARGE] = atoi(optarg); break;
            case 'S': params->cars_count[TYPE_SPECIAL] = atoi(optarg); break;
            case 'd': params->tick_ms = atoi(optarg); break;
            case 'R': params->seed = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 'L':
                if (strlen(optarg) == 0 || strlen(optarg) >= sizeof(params->log_path)) {
                    const char err[] = "Ошибка: недопустимая длина пути журнала\n";
                    write_all(STDERR_FILENO, err, sizeof(err) - 1);
                    return -1;
                }
                strcpy(params->log_path, optarg);
                break;
            case 'g': {
                int s = parse_strategy(optarg);
                if (s == -1) {
                    const char err[] = "Ошибка: неизвестная стратегия (допустимо: fifo, lifo, random, short)\n";
                    write_all(STDERR_FILENO, err, sizeof(err) - 1);
                    return -1;
                }
                params->strategy = (Strategy)s;
                break;
            }
            default: {
                const char usage[] = "Использование: <программа> [-t макс_такты (0 - без ограничения)] [-q очередь] [-r об_места] [-l кр_места] [-s сп_места] [-w ожидание] [-a мин_прибытие] [-b макс_прибытие] [-p мин_стоянка] [-P макс_стоянка] [-c об_авто] [-C кр_авто] [-S сп_авто] [-d мс_на_такт] [-R seed] [-g fifo|lifo|random|short] [-L файл_журнала]\n";
                write_all(STDERR_FILENO, usage, sizeof(usage) - 1);
                return -1;
            }
        }
    }

    // проверка параметров: отрицательные значения, min > max и такт короче 1 мс недопустимы
    bool bad_params = params->max_ticks < 0 || params->queue_capacity < 0 || params->max_wait_time < 0 ||
                      params->arrival_min < 0 || params->arrival_min > params->arrival_max ||
                      params->park_time_min < 0 || params->park_time_min > params->park_time_max ||
                      params->tick_ms < 1;
    for (int i = 0; i < 3; i++) {
        if (params->spots_count[i] < 0 || params->cars_count[i] < 0) bad_params = true;
    }
    long long total_spots = 0, total_cars = 0;
    for (int i = 0; i < 3; i++) {
        total_spots += params->spots_count[i];
        total_cars += params->cars_count[i];
    }
    if (total_spots > INT_MAX || total_cars > INT_MAX - 2 || optind != argc) bad_params = true;
    if (params->tick_ms > 0) {
        // Ограничиваем до умножения: наносекундные интервалы и абсолютные сроки не переполнятся.
        long long max_duration = LLONG_MAX / 2 / 1000000LL / params->tick_ms;
        if (params->max_ticks > max_duration || params->arrival_max > max_duration ||
            params->park_time_max > max_duration || params->max_wait_time > max_duration)
            bad_params = true;
    }
    if (bad_params) {
        const char err[] = "Ошибка: недопустимые параметры, диапазоны, длительности или лишние аргументы\n";
        write_all(STDERR_FILENO, err, sizeof(err) - 1);
        return -1;
    }
    return 0;
}
