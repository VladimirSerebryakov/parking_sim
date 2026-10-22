#ifndef PARAMS_H
#define PARAMS_H

#include "common.h"

#define DEFAULT_LOG_PATH "../parking_result.txt" // Файл журнала по умолчанию (относительно текущей директории)

// Параметры
typedef struct {
    int spots_count[3]; // Количество мест каждого типа
    int cars_count[3];  // Количество машин каждого типа для генерации
    int queue_capacity;  // Вместимость очереди
    int arrival_min, arrival_max; // Диапазон интервалов прибытия
    int park_time_min, park_time_max; // Диапазон времени стоянки
    int max_wait_time; // Максимальное время в очереди
    int max_ticks; // Предельное число тактов (0 - без ограничения)
    int tick_ms; // Длительность такта в миллисекундах
    unsigned int seed; // Начальное значение ГСЧ (по умолчанию - текущее время)
    Strategy strategy; // Стратегия назначения мест
    const char *log_path; // Путь к файлу журнала
} SimParams;

// Заполняет параметры значениями по умолчанию и аргументами командной строки.
// Возвращает 0 при успехе, -1 при ошибке (сообщение уже выведено в stderr)
int parse_params(int argc, char *argv[], SimParams *params);
// Название стратегии для вывода ("FIFO", "LIFO", ...)
const char *strategy_name(Strategy s);

#endif // PARAMS_H
