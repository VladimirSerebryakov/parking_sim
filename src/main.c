#include "parking.h"
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <signal.h>

volatile sig_atomic_t keep_running = 1;

// обработчик ctrl+c
void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand(time(NULL));

    signal(SIGINT, handle_sigint);

    SimParams params = {
        .spots_count = {2, 1, 1},
        .cars_count = {15, 12, 10},
        .queue_capacity = 3,
        .arrival_min = 0,
        .arrival_max = 2,
        .park_time_min = 7,
        .park_time_max = 10,
        .max_wait_time = 8,
        .max_ticks = 100
    };

    int opt;
    while ((opt = getopt(argc, argv, "t:q:r:l:s:w:a:b:p:P:c:C:S:")) != -1) {
        switch (opt) {
            case 't': params.max_ticks = atoi(optarg); break;
            case 'q': params.queue_capacity = atoi(optarg); break;
            case 'r': params.spots_count[TYPE_REGULAR] = atoi(optarg); break;
            case 'l': params.spots_count[TYPE_LARGE] = atoi(optarg); break;
            case 's': params.spots_count[TYPE_SPECIAL] = atoi(optarg); break;
            case 'w': params.max_wait_time = atoi(optarg); break;
            case 'a': params.arrival_min = atoi(optarg); break;
            case 'b': params.arrival_max = atoi(optarg); break;
            case 'p': params.park_time_min = atoi(optarg); break;
            case 'P': params.park_time_max = atoi(optarg); break;
            case 'c': params.cars_count[TYPE_REGULAR] = atoi(optarg); break;
            case 'C': params.cars_count[TYPE_LARGE] = atoi(optarg); break;
            case 'S': params.cars_count[TYPE_SPECIAL] = atoi(optarg); break;
            default: {
                const char usage[] = "Использование: ./parking_sim [-t макс_такты] [-q очередь] [-r об_места] [-l кр_места] [-s сп_места] [-w ожидание] [-a мин_прибытие] [-b макс_прибытие] [-p мин_стоянка] [-P макс_стоянка] [-c об_авто] [-C кр_авто] [-S сп_авто]\n";
                write(STDERR_FILENO, usage, sizeof(usage) - 1);
                return 1;
            }
        }
    }

    Dispatcher dispatcher;
    init_dispatcher(&dispatcher, &params);
    memset(dispatcher.event_history, 0, sizeof(dispatcher.event_history));

    write(STDOUT_FILENO, "\033[?1049h", 8);

    record_event(&dispatcher, "--- Старт симуляции ---");

    int next_arrival_timer = get_rand_range(params.arrival_min, params.arrival_max);
    int car_id_counter = 1;

    for (int tick = 0; tick < params.max_ticks && keep_running; tick++) {
        dispatcher.current_tick = tick;

        int total_cars_left = dispatcher.params.cars_count[TYPE_REGULAR] +
                              dispatcher.params.cars_count[TYPE_LARGE] +
                              dispatcher.params.cars_count[TYPE_SPECIAL];

        if (next_arrival_timer <= 0 && total_cars_left > 0) {
            int available_types[3];
            int available_count = 0;
            for (int i = 0; i < 3; i++) {
                if (dispatcher.params.cars_count[i] > 0) {
                    available_types[available_count++] = i;
                }
            }

            int rand_idx = get_rand_range(0, available_count - 1);
            CarType chosen_type = (CarType)available_types[rand_idx];
            dispatcher.params.cars_count[chosen_type]--;

            Car new_car = {
                .id = car_id_counter++,
                .type = chosen_type,
                .parking_time = get_rand_range(params.park_time_min, params.park_time_max),
                .parking_time_left = 0,
                .wait_time = 0,
                .max_wait_time = params.max_wait_time
            };

            const char *type_names[] = {"Обычный", "Крупногабаритный", "Специальный"};
            record_event(&dispatcher, "Прибытие: Автомобиль #%d (Тип: %s)",
                         new_car.id, type_names[new_car.type]);

            int spot_idx = find_free_spot(&dispatcher, new_car.type);
            if (spot_idx != -1) {
                dispatcher.spots[spot_idx].is_occupied = true;
                dispatcher.spots[spot_idx].current_car = new_car;
                dispatcher.spots[spot_idx].current_car.parking_time_left = new_car.parking_time;
                dispatcher.cars_served++;

                record_event(&dispatcher, "-> Назначено место #%d. Время стоянки: %d тактов",
                             dispatcher.spots[spot_idx].id, new_car.parking_time);
            } else {
                if (enqueue(&dispatcher.queue, new_car)) {
                    record_event(&dispatcher, "-> Нет мест. Постановка в очередь (размер: %d/%d)",
                                 dispatcher.queue.size, dispatcher.queue.capacity);
                } else {
                    dispatcher.cars_rejected++;
                    record_event(&dispatcher, "-> Очередь заполнена. Автомобиль уходит.");
                }
            }
            next_arrival_timer = get_rand_range(params.arrival_min, params.arrival_max);
        } else {
            next_arrival_timer--;
        }

        for (int i = 0; i < dispatcher.total_spots; i++) {
            if (dispatcher.spots[i].is_occupied) {
                dispatcher.spots[i].current_car.parking_time_left--;
                if (dispatcher.spots[i].current_car.parking_time_left <= 0) {
                    record_event(&dispatcher, "Окончание стоянки: Автомобиль #%d покинул место #%d",
                                 dispatcher.spots[i].current_car.id, dispatcher.spots[i].id);
                    dispatcher.spots[i].is_occupied = false;
                }
            }
        }

        int current_q_size = dispatcher.queue.size;
        for (int i = 0; i < current_q_size; i++) {
            Car queued_car;
            dequeue(&dispatcher.queue, &queued_car);

            queued_car.wait_time++;

            if (queued_car.wait_time > queued_car.max_wait_time) {
                dispatcher.cars_left_queue++;
                record_event(&dispatcher, "Уход из очереди: Автомобиль #%d устал ждать",
                             queued_car.id);
            } else {
                int spot_idx = find_free_spot(&dispatcher, queued_car.type);
                if (spot_idx != -1) {
                    dispatcher.spots[spot_idx].is_occupied = true;
                    dispatcher.spots[spot_idx].current_car = queued_car;
                    dispatcher.spots[spot_idx].current_car.parking_time_left = queued_car.parking_time;
                    dispatcher.cars_served++;

                    record_event(&dispatcher, "Назначение из очереди: Авто #%d занял место #%d",
                                 queued_car.id, dispatcher.spots[spot_idx].id);
                } else {
                    enqueue(&dispatcher.queue, queued_car);
                }
            }
        }

        draw_dashboard(&dispatcher);
        usleep(800000);
        bool is_parking_empty = true;
        for (int i = 0; i < dispatcher.total_spots; i++) {
            if (dispatcher.spots[i].is_occupied) {
                is_parking_empty = false;
                break;
            }
        }

        if (total_cars_left == 0 && is_queue_empty(&dispatcher.queue) && is_parking_empty) {
            record_event(&dispatcher, "Все автомобили обслужены. Завершение.");
            draw_dashboard(&dispatcher);
            usleep(1500000);
            break;
        }
    }

    if (!keep_running) {
        record_event(&dispatcher, "Симуляция прервана (Ctrl+C)");
        draw_dashboard(&dispatcher);
        usleep(1500000);
    }

    write(STDOUT_FILENO, "\033[?1049l", 8);

    const char* exit_reason = keep_running ? "СИМУЛЯЦИЯ ЗАВЕРШЕНА" : "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)";

    char final_stats[512];
    int len = snprintf(final_stats, sizeof(final_stats),
        "\n========================================\n"
        " %s\n"
        "========================================\n"
        " ИТОГОВАЯ СТАТИСТИКА:\n"
        " - Обслужено автомобилей: \033[32m%d\033[0m\n"
        " - Ушло по таймауту из очереди: \033[31m%d\033[0m\n"
        " - Отказано (очередь заполнена): \033[31m%d\033[0m\n"
        "========================================\n\n",
        exit_reason, dispatcher.cars_served, dispatcher.cars_left_queue, dispatcher.cars_rejected);

    write(STDOUT_FILENO, final_stats, len);

    char file_stats[512];
    int file_len = snprintf(file_stats, sizeof(file_stats),
        "\n========================================\n"
        " %s\n"
        "========================================\n"
        " ИТОГОВАЯ СТАТИСТИКА:\n"
        " - Обслужено автомобилей: %d\n"
        " - Ушло по таймауту из очереди: %d\n"
        " - Отказано (очередь заполнена): %d\n"
        "========================================\n",
        exit_reason, dispatcher.cars_served, dispatcher.cars_left_queue, dispatcher.cars_rejected);

    if (dispatcher.log_fd != -1) {
        write(dispatcher.log_fd, file_stats, file_len);
    }

    cleanup_dispatcher(&dispatcher);

    return 0;
}
