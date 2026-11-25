#include "protocol.h"
#include <unistd.h>

// Получение ответа B не должно менять порядок двух уведомлений для A.
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    Transport owner;
    if (transport_create(&owner, argv[1]) < 0) return 1;
    int result = 1;
    MqClient a = { .pid = getpid(), .cookie = 1 }, b = { .pid = getpid(), .cookie = 2 };
    if (transport_publish(&owner) < 0) goto close_owner;
    if (transport_connect(&a.transport, argv[1]) < 0) goto close_owner;
    if (transport_connect(&b.transport, argv[1]) < 0) goto close_a;
    Message messages[] = {
        { .magic = MQ_MAGIC, .pid = getpid(), .cookie = 1, .type = MQ_STATUS, .car.status = CAR_QUEUED },
        { .magic = MQ_MAGIC, .pid = getpid(), .cookie = 2, .type = MQ_ACK },
        { .magic = MQ_MAGIC, .pid = getpid(), .cookie = 1, .type = MQ_STATUS, .car.status = CAR_PARKED }
    };
    for (int i = 0; i < 3; i++) if (transport_send(owner.replies, &messages[i]) < 0) goto close_b;
    if (client_poll(&b) != 1 || b.last.type != MQ_ACK) goto close_b;
    if (client_poll(&a) != 1 || a.status != CAR_QUEUED) goto close_b;
    if (client_poll(&a) != 1 || a.status != CAR_PARKED) goto close_b;
    if (client_poll(&a) != 0 || client_poll(&b) != 0) goto close_b;
    result = 0;
close_b:
    client_close(&b);
close_a:
    client_close(&a);
close_owner:
    transport_close(&owner);
    return result;
}
