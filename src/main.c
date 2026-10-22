#include "parking.h"
#include "params.h"
#include "ui.h"
#include "sim_clock.h"
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>

// флаг остановки: выставляется по ctrl+c, проверяется в цикле и во время сна
static atomic_bool stop_requested = false;

// обработчик ctrl+c
static void handle_sigint(int sig) {
    (void)sig;
    atomic_store(&stop_requested, true);
}

// пауза, чтобы успеть увидеть последний кадр: 2 такта, но не больше 1.5 с
static void final_pause(int tick_ms) {
    int pause_ms = tick_ms < 750 ? tick_ms * 2 : 1500;
    usleep(pause_ms * 1000);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    SimParams params;
    if (parse_params(argc, argv, &params) != 0) {
        return 1;
    }

    unsigned int rng = params.seed; // состояние ГСЧ (при одинаковом seed прогон повторяется)
    sim_clock_start(params.tick_ms);

    Parking parking;
    init_parking(&parking, &params);

    ui_enter();

    record_event(&parking, "--- Старт симуляции (seed: %u, стратегия: %s) ---",
                 params.seed, strategy_name(params.strategy));

    int next_arrival_timer = get_rand_range(&rng, params.arrival_min, params.arrival_max);
    int car_id_counter = 1;

    // max_ticks == 0 - без ограничения по времени (до обработки всех авто или ctrl+c)
    for (int tick = 0; (params.max_ticks == 0 || tick < params.max_ticks) && !atomic_load(&stop_requested); tick++) {
        parking.current_tick = tick;

        int total_cars_left = parking.params.cars_count[TYPE_REGULAR] +
                              parking.params.cars_count[TYPE_LARGE] +
                              parking.params.cars_count[TYPE_SPECIAL];

        if (next_arrival_timer <= 0 && total_cars_left > 0) {
            int available_types[3];
            int available_count = 0;
            for (int i = 0; i < 3; i++) {
                if (parking.params.cars_count[i] > 0) {
                    available_types[available_count++] = i;
                }
            }

            int rand_idx = get_rand_range(&rng, 0, available_count - 1);
            CarType chosen_type = (CarType)available_types[rand_idx];
            parking.params.cars_count[chosen_type]--;

            Car new_car = {
                .id = car_id_counter++,
                .type = chosen_type,
                .parking_time = get_rand_range(&rng, params.park_time_min, params.park_time_max),
                .parking_time_left = 0,
                .wait_time = 0,
                .max_wait_time = params.max_wait_time
            };

            const char *type_names[] = {"Обычный", "Крупногабаритный", "Специальный"};
            record_event(&parking, "Прибытие: Автомобиль #%d (Тип: %s)",
                         new_car.id, type_names[new_car.type]);

            int spot_idx = find_free_spot(&parking, new_car.type);
            if (spot_idx != -1) {
                parking.spots[spot_idx].is_occupied = true;
                parking.spots[spot_idx].current_car = new_car;
                // +1, так как счётчик стоянки уменьшается уже в этом такте (у авто из очереди - со следующего)
                parking.spots[spot_idx].current_car.parking_time_left = new_car.parking_time + 1;
                parking.cars_served++;

                record_event(&parking, "-> Назначено место #%d. Время стоянки: %d тактов",
                             parking.spots[spot_idx].id, new_car.parking_time);
            } else {
                if (enqueue(&parking.queue, new_car)) {
                    record_event(&parking, "-> Нет мест. Постановка в очередь (размер: %d/%d)",
                                 parking.queue.size, parking.queue.capacity);
                } else {
                    parking.cars_rejected++;
                    record_event(&parking, "-> Очередь заполнена. Автомобиль уходит.");
                }
            }
            next_arrival_timer = get_rand_range(&rng, params.arrival_min, params.arrival_max);
        } else {
            next_arrival_timer--;
        }

        for (int i = 0; i < parking.total_spots; i++) {
            if (parking.spots[i].is_occupied) {
                parking.spots[i].current_car.parking_time_left--;
                if (parking.spots[i].current_car.parking_time_left <= 0) {
                    record_event(&parking, "Окончание стоянки: Автомобиль #%d покинул место #%d",
                                 parking.spots[i].current_car.id, parking.spots[i].id);
                    parking.spots[i].is_occupied = false;
                }
            }
        }

        int current_q_size = parking.queue.size;
        for (int i = 0; i < current_q_size; i++) {
            Car queued_car;
            dequeue(&parking.queue, &queued_car);

            queued_car.wait_time++;

            if (queued_car.wait_time > queued_car.max_wait_time) {
                parking.cars_left_queue++;
                record_event(&parking, "Уход из очереди: Автомобиль #%d устал ждать",
                             queued_car.id);
            } else {
                int spot_idx = find_free_spot(&parking, queued_car.type);
                if (spot_idx != -1) {
                    parking.spots[spot_idx].is_occupied = true;
                    parking.spots[spot_idx].current_car = queued_car;
                    parking.spots[spot_idx].current_car.parking_time_left = queued_car.parking_time;
                    parking.cars_served++;

                    record_event(&parking, "Назначение из очереди: Авто #%d занял место #%d",
                                 queued_car.id, parking.spots[spot_idx].id);
                } else {
                    enqueue(&parking.queue, queued_car);
                }
            }
        }

        draw_dashboard(&parking);
        sleep_ticks(1, &stop_requested);
        bool is_parking_empty = true;
        for (int i = 0; i < parking.total_spots; i++) {
            if (parking.spots[i].is_occupied) {
                is_parking_empty = false;
                break;
            }
        }

        if (total_cars_left == 0 && is_queue_empty(&parking.queue) && is_parking_empty) {
            record_event(&parking, "Все автомобили обслужены. Завершение.");
            draw_dashboard(&parking);
            final_pause(params.tick_ms);
            break;
        }
    }

    bool interrupted = atomic_load(&stop_requested);
    if (interrupted) {
        record_event(&parking, "Симуляция прервана (Ctrl+C)");
        draw_dashboard(&parking);
        final_pause(params.tick_ms);
    }

    ui_leave();

    const char *exit_reason = interrupted ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" : "СИМУЛЯЦИЯ ЗАВЕРШЕНА";
    print_final_stats(&parking, exit_reason);

    cleanup_parking(&parking);

    return 0;
}
