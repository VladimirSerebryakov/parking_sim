#include "protocol.h"
#include "io.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

static int check_client(MqClient *c, const sigset_t *signals) {
    int sig = model_stop_signal(signals);
    if (sig && client_call(c, MQ_STOP_REQUEST, sig, NULL) < 0) return -1;
    int rc = client_poll(c);
    if (rc < 0) return -1;
    if (rc == 0 && !transport_alive(&c->transport)) { errno = EPIPE; return -1; }
    return rc;
}
static int drive(MqClient *c, const sigset_t *signals) {
    // После получения плана автомобиль сам сообщает диспетчеру номер и тип.
    if (client_call(c, MQ_ARRIVE, 0, NULL) < 0) return -1;
    for (;;) {
        if (check_client(c, signals) < 0) return -1;
        if (c->has_status) {
            c->has_status = false;
            if (c->status == CAR_PARKED) break;
            if (c->status == CAR_REJECTED || c->status == CAR_TIMEOUT || c->status == CAR_CANCELLED)
                return client_disconnect(c);
        }
        sleep_until(monotonic_ns() + 1000000LL);
    }
    if (client_call(c, MQ_START, 0, NULL) < 0) return -1;
    long long end = monotonic_ns() + ticks_ns(&c->welcome.clock, c->welcome.car.parking_time);
    while (!c->stop && monotonic_ns() < end) {
        if (check_client(c, signals) < 0) return -1;
        long long wake = monotonic_ns() + 10000000LL;
        sleep_until(wake < end ? wake : end);
    }
    if (client_call(c, MQ_LEAVE, 0, NULL) < 0) return -1;
    return client_disconnect(c);
}
int main(int argc, char **argv) {
    sigset_t signals;
    if (model_block_signals(&signals) < 0) { report_error("sigprocmask"); return 1; }
    if (argc != 3) { errno = EINVAL; report_error("использование: parking_car СЕАНС НОМЕР"); return 1; }
    char *end; errno = 0;
    long id = strtol(argv[2], &end, 10);
    if (errno || !*argv[2] || *end || id < 1 || id > INT_MAX) { errno = EINVAL; report_error("номер автомобиля"); return 1; }
    MqClient client;
    if (client_connect(&client, argv[1], (int)id) < 0) {
        if (errno == ECANCELED) return 0;
        report_error("подключение MQ-автомобиля"); return 1;
    }
    int rc = drive(&client, &signals);
    if (rc < 0) {
        report_error("протокол автомобиля");
        if (transport_alive(&client.transport)) client_call(&client, MQ_FAIL, 0, NULL);
    }
    client_close(&client);
    return rc == 0 ? 0 : 1;
}
