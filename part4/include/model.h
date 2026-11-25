#ifndef PARKING_TCP_MODEL_H
#define PARKING_TCP_MODEL_H
#include "parking.h"
typedef struct {
    bool arrived, finished; // События учитываются ровно один раз
    int interval; // Плановый интервал генератора
    long long deadline, park_end; // Монотонные сроки сервера
} ModelCar;
typedef struct {
    Parking parking; // Полностью локальная модель
    ModelCar *slots; // Не зависят от соединений
    unsigned int rng; // ГСЧ стратегии
    unsigned long revision; // Версия состояния
} Model;
int model_create(Model *m, const SimParams *params);
void model_destroy(Model *m);
void model_start(Model *m);
int model_arrive(Model *m, int id, int type);
void model_advance(Model *m);
void model_stop(Model *m);
bool model_done(const Model *m);
void model_state(const Model *m, int id, char *text, size_t size);
void model_summary(const Model *m, const char *reason, char *text, size_t size);
#endif
