#include "parking.h"
#include "params.h"
#include "ui.h"
#include "sim_clock.h"
#include "threads.h"
#include <errno.h>
#include <signal.h>
#include <unistd.h>

// пауза, чтобы успеть увидеть последний кадр: 2 такта, но не больше 1.5 с
static void final_pause(int tick_ms) {
    int pause_ms = tick_ms < 750 ? tick_ms * 2 : 1500;
    usleep(pause_ms * 1000);
}

int main(int argc, char *argv[]) {
    // ctrl+c и SIGTERM обрабатывает отдельный поток; до его запуска сигналы просто ждут
    block_signals();

    SimParams params;
    if (parse_params(argc, argv, &params) != 0) {
        return 1;
    }

    sim_clock_start(params.tick_ms);
    Parking parking;
    init_parking(&parking, &params);

    // момент, когда истечёт лимит времени (если он задан)
    struct timespec time_limit;
    deadline_after(params.max_ticks, &time_limit);

    ui_enter();

    record_event(&parking, "--- Старт симуляции (seed: %u, стратегия: %s) ---",
                 params.seed, strategy_name(params.strategy));

    threads_start(&parking);

    // ждём одного из условий завершения на семафоре sim_end:
    // его поднимет последний завершившийся автомобиль или поток сигналов,
    // а лимит времени - это просто таймаут ожидания (max_ticks == 0 - ждём без лимита)
    int rc;
    if (params.max_ticks == 0) {
        while ((rc = sem_wait(&parking.sim_end)) == -1 && errno == EINTR) {}
    } else {
        while ((rc = sem_timedwait(&parking.sim_end, &time_limit)) == -1 && errno == EINTR) {}
    }
    bool time_over = rc == -1; // ETIMEDOUT

    // останавливаем всех и дожидаемся, пока каждый поток закончит работу
    atomic_store(&parking.stop, true);
    threads_join(&parking);

    int stop_signal = atomic_load(&parking.stop_signal);
    const char *exit_reason;
    if (stop_signal != 0) {
        record_event(&parking, "Симуляция прервана сигналом");
        exit_reason = stop_signal == SIGINT ? "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" : "СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)";
    } else if (time_over) {
        record_event(&parking, "Достигнут лимит времени. Завершение.");
        exit_reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)";
    } else {
        record_event(&parking, "Все автомобили обслужены. Завершение.");
        exit_reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА";
    }
    draw_dashboard(&parking);
    final_pause(params.tick_ms);

    ui_leave();

    print_final_stats(&parking, exit_reason);

    cleanup_parking(&parking);

    return 0;
}
