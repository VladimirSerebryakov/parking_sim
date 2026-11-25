#include "model.h"
#include "client.h"
#include "io.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

// Предметный сценарий проверяется фактической моделью, без эмуляции сети.
int main(int argc, char **argv) {
    assert(argc == 2);
    SimParams p = { .spots_count = {1, 0, 0}, .cars_count = {5, 0, 0}, .queue_capacity = 1,
        .tick_ms = 1, .seed = 7, .park_time_min = 100000, .park_time_max = 100000,
        .max_wait_time = 100000, .strategy = STRATEGY_FIFO };
    assert(strlen(argv[1]) < sizeof(p.log_path)); strcpy(p.log_path, argv[1]);
    Model m; assert(model_create(&m, &p) == 0);
    assert(m.parking.state->clock.start_ns == 0);
    assert(model_arrive(&m, 1, TYPE_LARGE) == -1);
    assert(model_arrive(&m, 1, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.cars[0].status == CAR_PARKED);
    assert(model_arrive(&m, 1, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.state->cars_generated == 1 && m.parking.state->cars_served == 1);
    assert(model_arrive(&m, 2, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.cars[1].status == CAR_QUEUED);
    assert(model_arrive(&m, 3, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.cars[2].status == CAR_REJECTED);
    m.slots[1].deadline = 1; model_advance(&m);
    assert(m.parking.cars[1].status == CAR_TIMEOUT);
    assert(model_arrive(&m, 4, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.cars[3].status == CAR_QUEUED);
    m.slots[0].park_end = 1; model_advance(&m);
    assert(m.parking.cars[0].status == CAR_DONE && m.parking.cars[3].status == CAR_PARKED);
    assert(model_arrive(&m, 5, TYPE_REGULAR) == 0); model_advance(&m);
    assert(m.parking.cars[4].status == CAR_QUEUED);
    model_stop(&m); assert(model_done(&m));
    assert(!m.parking.state->queue.size && m.parking.spots[0].car_idx == -1);
    char summary[1024]; model_summary(&m, "СИМУЛЯЦИЯ ЗАВЕРШЕНА", summary, sizeof(summary));
    assert(strstr(summary, "Обслужено автомобилей: 2"));
    // Сохранение результата после повторной команды не зависит от соединения.
    assert(model_arrive(&m, 1, TYPE_REGULAR) == 0 && m.parking.state->cars_finished == 5);
    assert(m.parking.state->cars_cancelled == 1);
    assert(write_all(m.parking.log_fd, summary, strlen(summary)) == 0);
    long long values[2];
    assert(text_numbers("1 2", values, 2) == 0);
    assert(text_numbers("1 2 3", values, 2) == -1);
    assert(text_numbers("99999999999999999999999 0", values, 2) == -1);
    assert(text_numbers("1x 2", values, 2) == -1);
    model_destroy(&m);
    return 0;
}
