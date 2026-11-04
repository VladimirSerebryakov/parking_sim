#include "unnamed_resources.h"
#include "io.h"
#include "ui.h"
#include <unistd.h>

int main(int argc, char *argv[]) {
    sigset_t signals;
    if (block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    SimParams params;
    if (parse_params(argc, argv, &params) != 0) return 1;
    UnnamedResources resources;
    if (create_unnamed_resources(&resources, &params) < 0) {
        report_error("инициализация разделяемой памяти, семафоров или журнала");
        return 1;
    }
    Parking *p = &resources.processes.parking;
    record_event(p, "--- Старт симуляции (seed: %u, стратегия: %s) ---",
                 params.seed, strategy_name(params.strategy));
    record_event(p, "POSIX память: %s (имя удалено после mmap)", resources.memory.name);
    record_event(p, "Синхронизация: неименованные POSIX семафоры в общей памяти");
    record_event(p, "Параметры: места %d/%d/%d; очередь %d",
                 params.spots_count[0], params.spots_count[1], params.spots_count[2], params.queue_capacity);
    ui_enter();
    const char *reason = "СИМУЛЯЦИЯ ЗАВЕРШЕНА (ошибка запуска)";
    int rc = run_processes(&resources.processes, &signals, &reason);
    // После аварии ребёнка общее состояние может быть неполным: статистику не выдаём за корректную.
    if (rc != 2) {
        draw_dashboard(p);
        int pause_ms = params.tick_ms < 750 ? params.tick_ms * 2 : 1500;
        sleep_until(monotonic_ns() + (long long)pause_ms * 1000000LL);
    }
    ui_leave();
    if (rc != 2) print_final_stats(p, reason);
    destroy_unnamed_resources(&resources);
    return rc == 0 ? 0 : 1;
}
