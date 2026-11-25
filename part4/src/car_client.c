#include "client.h"
#include "common.h"
#include "sim_clock.h"
#include <errno.h>
#include <stdio.h>

int car_drive(Client *c) {
    bool arrival_requested = false;
    int last = -1;
    long long leave_at = 0;
    while (!tcp_interrupted && !c->stopped) {
        NetFrame f;
        int rc = client_next(c, &f, 10);
        if (rc < 0) return -1;
        if (rc == 2) return 0;
        if (rc == 1) {
            long long v[7];
            bool valid = f.type == NET_STATE && text_numbers(f.body, v, 7) == 0 &&
                v[0] == c->id && v[1] == c->type && v[2] >= CAR_ARRIVING && v[2] <= CAR_DONE &&
                v[3] >= 0 && v[3] <= 2048 && v[4] >= 0 && v[4] <= 2147483647 &&
                v[5] >= 0 && v[5] <= 4611686018427LL && v[6] >= 0;
            if (valid) {
                bool assigned = v[2] == CAR_PARKED || v[2] == CAR_DONE;
                valid = assigned ? v[3] > 0 : v[3] == 0;
            }
            net_frame_free(&f);
            if (!valid) { errno = EPROTO; return -1; }
            static const char *names[] = {"ожидание решения", "очередь", "стоянка", "отказ", "таймаут", "отмена", "отъезд"};
            int status = (int)v[2];
            char text[256];
            if (last != status) {
                snprintf(text, sizeof(text), "Авто #%d: %s, место %lld, такт %lld\n", c->id, names[status], v[3], v[6]);
                client_print(text); last = status;
            }
            if (status >= CAR_REJECTED) return 0;
            if (status == CAR_ARRIVING && !arrival_requested) {
                // HELLO не является прибытием. У восстановленного авто ARRIVE уже принят.
                snprintf(text, sizeof(text), "%d %d", c->id, c->type);
                if (client_send(c, NET_ARRIVE, text) < 0) return -1;
                arrival_requested = true;
            }
            if (status == CAR_PARKED) leave_at = monotonic_ns() + v[5] * 1000000LL;
        }
        if (leave_at && monotonic_ns() >= leave_at) {
            if (client_send(c, NET_LEAVE, "") < 0) return -1;
            leave_at = 0;
        }
    }
    return 0;
}
