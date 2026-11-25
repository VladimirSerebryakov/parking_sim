#include "threads.h"
#include "sim_clock.h"
#include "strategy.h"
#include "ui.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static Parking *parking; // Общая парковка для всех потоков (задаётся до их запуска)
static pthread_t generator_tid; // Поток-генератор
static pthread_t dispatcher_tid; // Поток-диспетчер
static pthread_t render_tid; // Поток-отрисовщик
static pthread_t signal_tid; // Поток обработки сигналов
static sigset_t handled_signals; // SIGINT, SIGTERM (остановка) и SIGUSR1 (сигнальному потоку пора выходить)

static const char *type_names[] = {"Обычный", "Крупногабаритный", "Специальный"};

// sem_wait, повторяемый, если ожидание прервал сигнал
static void sem_wait_retry(sem_t *sem) {
    while (sem_wait(sem) == -1 && errno == EINTR) {}
}

// Если генератор закончил и все выпущенные авто завершились - будим main (под мьютексом)
static void notify_if_all_done(Parking *p) {
    if (!p->end_notified && p->generator_done && p->cars_finished == p->cars_generated) {
        p->end_notified = true;
        sem_post(&p->sim_end);
    }
}

// Автомобиль завершил работу (уехал, получил отказ или отмену) (под мьютексом)
static void car_finished(Parking *p) {
    p->cars_finished++;
    notify_if_all_done(p);
}

/* ===================== Диспетчер ===================== */
// Все функции этого раздела вызываются под мьютексом парковки.
// Решение для машины диспетчер записывает в car->status и сразу делает sem_post(&car->granted) -
// тоже под мьютексом, поэтому машина, увидевшая новый статус, может быть уверена, что post уже сделан.

// Назначает машине место и сообщает ей об этом
static void assign_spot(Parking *p, Car *car, int spot_idx, bool from_queue) {
    ParkingSpot *spot = &p->spots[spot_idx];
    spot->is_occupied = true;
    spot->current_car = car;
    car->spot_idx = spot_idx;
    car->leave_tick = now_ticks() + car->parking_time;
    car->status = CAR_PARKED;
    p->cars_served++;
    if (from_queue) {
        record_event(p, "Диспетчер: место #%d назначено авто #%d из очереди (ждало %d т.)",
                     spot->id, car->id, now_ticks() - car->arrival_tick);
    } else {
        record_event(p, "Диспетчер: авто #%d назначено место #%d", car->id, spot->id);
    }
    sem_post(&car->granted);
}

// Свободные места отдаём ожидающим в очереди - по выбранной стратегии
static void serve_queue(Parking *p, unsigned int *rng) {
    for (int i = 0; i < p->total_spots && !is_queue_empty(&p->queue); i++) {
        if (p->spots[i].is_occupied) continue;
        int idx = pick_car(&p->queue, p->spots[i].type, p->params.strategy, rng);
        if (idx == -1) continue; // никто в очереди не подходит к этому месту
        Car *car = queue_get(&p->queue, idx);
        queue_remove_at(&p->queue, idx);
        assign_spot(p, car, i, true);
    }
}

// Решаем судьбу только что прибывших: место, очередь или отказ
static void serve_inbox(Parking *p) {
    Car *car;
    while (dequeue(&p->inbox, &car)) {
        int spot_idx = find_free_spot(p, car->type);
        if (spot_idx != -1) {
            assign_spot(p, car, spot_idx, false);
        } else if (enqueue(&p->queue, car)) {
            car->status = CAR_QUEUED;
            record_event(p, "Диспетчер: мест нет, авто #%d поставлено в очередь (%d/%d)",
                         car->id, p->queue.size, p->queue.capacity);
            sem_post(&car->granted);
        } else {
            car->status = CAR_REJECTED;
            p->cars_rejected++;
            record_event(p, "Диспетчер: очередь заполнена, авто #%d уезжает", car->id);
            sem_post(&car->granted);
        }
    }
}

// Остановка: всем, кто ещё ждёт решения или места, сообщаем об отмене
static void cancel_waiting(Parking *p) {
    Car *car;
    while (dequeue(&p->inbox, &car) || dequeue(&p->queue, &car)) {
        car->status = CAR_CANCELLED;
        p->cars_cancelled++;
        record_event(p, "Диспетчер: остановка, ожидание авто #%d прервано", car->id);
        sem_post(&car->granted);
    }
}

// Поток-диспетчер: спит, пока не случится событие, затем раздаёт места
static void *dispatcher_thread(void *arg) {
    (void)arg;
    Parking *p = parking;
    unsigned int rng = p->params.seed + 1; // свой ГСЧ для стратегии RANDOM

    for (;;) {
        sem_wait_retry(&p->dispatcher_wake); // ждём: прибытие, освобождение места или остановка

        pthread_mutex_lock(&p->mx);
        if (atomic_load(&p->stop)) {
            cancel_waiting(p);
            pthread_mutex_unlock(&p->mx);
            break;
        }
        serve_queue(p, &rng); // сначала очередь: эти машины ждут дольше только что прибывших
        serve_inbox(p);
        pthread_mutex_unlock(&p->mx);
    }
    return NULL;
}

/* ===================== Автомобиль ===================== */

// Машина в очереди не дождалась места: убирает себя из очереди (под мьютексом)
static void leave_queue(Parking *p, Car *car) {
    for (int i = 0; i < p->queue.size; i++) {
        if (queue_get(&p->queue, i) == car) {
            queue_remove_at(&p->queue, i);
            break;
        }
    }
    car->status = CAR_TIMEOUT;
    p->cars_left_queue++;
    record_event(p, "Уход из очереди: авто #%d не дождалось места за %d т.",
                 car->id, now_ticks() - car->arrival_tick);
}

// Поток автомобиля: сообщает о себе диспетчеру, ждёт решения, стоит, уезжает
static void *car_thread(void *arg) {
    Car *car = (Car *)arg;
    Parking *p = parking;

    // 1. Прибытие: встаём во «входящие» и будим диспетчера
    pthread_mutex_lock(&p->mx);
    car->arrival_tick = now_ticks();
    record_event(p, "Прибытие: авто #%d (%s) сообщает диспетчеру номер и тип", car->id, type_names[car->type]);
    if (atomic_load(&p->stop)) {
        // симуляция уже останавливается - диспетчер может не ответить, поэтому не ждём
        car->status = CAR_CANCELLED;
        p->cars_cancelled++;
        car_finished(p);
        record_event(p, "Остановка: авто #%d не въезжает", car->id);
        pthread_mutex_unlock(&p->mx);
        return NULL;
    }
    car->status = CAR_ARRIVING;
    enqueue(&p->inbox, car);
    pthread_mutex_unlock(&p->mx);
    sem_post(&p->dispatcher_wake);

    // время ожидания отсчитывается от момента прибытия
    struct timespec deadline;
    deadline_after(car->max_wait_time, &deadline);

    // 2. Ждём первого ответа диспетчера: место, очередь, отказ или отмена
    sem_wait_retry(&car->granted);
    pthread_mutex_lock(&p->mx);
    CarStatus status = car->status;
    pthread_mutex_unlock(&p->mx);

    // 3. В очереди: ждём места, но не дольше max_wait_time
    if (status == CAR_QUEUED) {
        int rc;
        while ((rc = sem_timedwait(&car->granted, &deadline)) == -1 && errno == EINTR) {}

        pthread_mutex_lock(&p->mx);
        if (rc == -1 && car->status == CAR_QUEUED) {
            // время вышло, а места так и не дали - уходим
            leave_queue(p, car);
            car_finished(p);
            pthread_mutex_unlock(&p->mx);
            return NULL;
        }
        if (rc == -1) {
            // таймаут совпал с решением диспетчера: статус уже не QUEUED, значит, post уже сделан -
            // забираем его, не блокируясь (под мьютексом блокирующее ожидание запрещено)
            sem_trywait(&car->granted);
        }
        status = car->status;
        pthread_mutex_unlock(&p->mx);
    }

    // отказ или отмена: уезжаем, не заехав
    if (status != CAR_PARKED) {
        pthread_mutex_lock(&p->mx);
        car_finished(p);
        pthread_mutex_unlock(&p->mx);
        return NULL;
    }

    // 4. Стоянка: спим без мьютекса, чтобы остальные потоки могли работать
    pthread_mutex_lock(&p->mx);
    ParkingSpot *spot = &p->spots[car->spot_idx];
    record_event(p, "Начало стоянки: авто #%d на месте #%d (%d т.)", car->id, spot->id, car->parking_time);
    pthread_mutex_unlock(&p->mx);

    bool parked_full_time = sleep_ticks(car->parking_time, &p->stop);

    // 5. Отъезд: освобождаем место и сообщаем диспетчеру, что его можно отдать
    pthread_mutex_lock(&p->mx);
    if (parked_full_time) {
        record_event(p, "Окончание стоянки: авто #%d покидает место #%d", car->id, spot->id);
    } else {
        record_event(p, "Остановка: авто #%d досрочно покидает место #%d", car->id, spot->id);
    }
    spot->is_occupied = false;
    spot->current_car = NULL;
    car->status = CAR_DONE;
    record_event(p, "Освобождение места #%d (%s)", spot->id, type_names[spot->type]);
    car_finished(p);
    pthread_mutex_unlock(&p->mx);
    sem_post(&p->dispatcher_wake);
    return NULL;
}

/* ===================== Генератор и отрисовщик ===================== */

// Поток-генератор: через случайные интервалы выпускает автомобили
static void *generator_thread(void *arg) {
    (void)arg;
    Parking *p = parking;
    unsigned int rng = p->params.seed; // свой ГСЧ: типы, интервалы и время стоянки выбирает только этот поток

    // сколько машин каждого типа осталось выпустить
    int cars_left[3];
    for (int i = 0; i < 3; i++) cars_left[i] = p->params.cars_count[i];

    for (int i = 0; i < p->total_cars; i++) {
        int interval = get_rand_range(&rng, p->params.arrival_min, p->params.arrival_max);
        if (!sleep_ticks(interval, &p->stop)) break; // остановка - больше никого не выпускаем

        // выбираем случайный тип из тех, что ещё остались
        int available_types[3];
        int available_count = 0;
        for (int t = 0; t < 3; t++) {
            if (cars_left[t] > 0) {
                available_types[available_count++] = t;
            }
        }
        CarType chosen_type = (CarType)available_types[get_rand_range(&rng, 0, available_count - 1)];
        cars_left[chosen_type]--;

        // машина живёт в общем массиве, а не в локальной переменной: поток получает её адрес
        Car *car = &p->cars[i];
        car->id = i + 1;
        car->type = chosen_type;
        car->parking_time = get_rand_range(&rng, p->params.park_time_min, p->params.park_time_max);
        car->max_wait_time = p->params.max_wait_time;
        sem_init(&car->granted, 0, 0);

        if (pthread_create(&car->thread, NULL, car_thread, car) != 0) {
            sem_destroy(&car->granted);
            const char err[] = "Ошибка: не удалось создать поток автомобиля\n";
            write(STDERR_FILENO, err, sizeof(err) - 1);
            break;
        }
        pthread_mutex_lock(&p->mx);
        p->cars_generated++;
        pthread_mutex_unlock(&p->mx);
    }

    pthread_mutex_lock(&p->mx);
    p->generator_done = true;
    notify_if_all_done(p); // на случай, если все машины уже уехали (или их не было вовсе)
    pthread_mutex_unlock(&p->mx);
    return NULL;
}

// Поток-отрисовщик: раз в такт перерисовывает дашборд
static void *render_thread(void *arg) {
    (void)arg;
    Parking *p = parking;
    while (!atomic_load(&p->stop)) {
        // текст собираем под мьютексом (быстро), а выводим уже без него (вывод в терминал медленный)
        pthread_mutex_lock(&p->mx);
        int len;
        char *screen = build_dashboard(p, &len);
        pthread_mutex_unlock(&p->mx);

        if (screen != NULL) {
            write(STDOUT_FILENO, screen, len);
            free(screen);
        }
        sleep_ticks(1, &p->stop);
    }
    return NULL;
}

/* ===================== Сигналы ===================== */

// Поток сигналов: синхронно ждёт сигналы через sigwait. Во всех остальных потоках они заблокированы,
// поэтому асинхронный обработчик не нужен и в реакции на сигнал можно спокойно брать мьютекс
static void *signal_thread(void *arg) {
    (void)arg;
    Parking *p = parking;
    for (;;) {
        int sig;
        if (sigwait(&handled_signals, &sig) != 0) continue;
        if (sig == SIGUSR1) break; // main завершает программу

        // SIGINT или SIGTERM: останавливаем симуляцию (повторные сигналы игнорируем)
        if (atomic_load(&p->stop_signal) == 0) {
            pthread_mutex_lock(&p->mx);
            record_event(p, "Получен сигнал %s - остановка симуляции", sig == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM");
            pthread_mutex_unlock(&p->mx);
            atomic_store(&p->stop_signal, sig);
            atomic_store(&p->stop, true);
            sem_post(&p->sim_end);
        }
    }
    return NULL;
}

/* ===================== Управление потоками ===================== */

void block_signals(void) {
    // блокируем до создания потоков: маску унаследуют все, и сигналы будет получать только signal_thread
    sigemptyset(&handled_signals);
    sigaddset(&handled_signals, SIGINT);
    sigaddset(&handled_signals, SIGTERM);
    sigaddset(&handled_signals, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &handled_signals, NULL);
}

void threads_start(Parking *p) {
    parking = p;
    pthread_create(&signal_tid, NULL, signal_thread, NULL);
    pthread_create(&dispatcher_tid, NULL, dispatcher_thread, NULL);
    pthread_create(&generator_tid, NULL, generator_thread, NULL);
    pthread_create(&render_tid, NULL, render_thread, NULL);
}

void threads_join(Parking *p) {
    // будим диспетчера, чтобы он увидел stop и отменил ожидание всем в очереди
    sem_post(&p->dispatcher_wake);

    // сначала генератор: после него число запущенных машин больше не меняется
    pthread_join(generator_tid, NULL);

    pthread_mutex_lock(&p->mx);
    int generated = p->cars_generated;
    pthread_mutex_unlock(&p->mx);

    for (int i = 0; i < generated; i++) {
        pthread_join(p->cars[i].thread, NULL);
    }
    pthread_join(dispatcher_tid, NULL);
    pthread_join(render_tid, NULL);

    // сигнальный поток ждёт в sigwait - отправляем ему SIGUSR1, чтобы он вышел
    pthread_kill(signal_tid, SIGUSR1);
    pthread_join(signal_tid, NULL);
}
