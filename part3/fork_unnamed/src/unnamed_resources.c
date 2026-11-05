#include "unnamed_resources.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

_Static_assert(_Alignof(sem_t) <= _Alignof(max_align_t), "Выравнивание области семафоров");

int create_unnamed_resources(UnnamedResources *r, const SimParams *params) {
    *r = (UnnamedResources){ .memory = { .parking = { .log_fd = -1 } } };
    size_t count = (size_t)params->cars_count[0] + params->cars_count[1] + params->cars_count[2] + 3;
    if (count > SIZE_MAX / sizeof(sem_t) || count > SIZE_MAX / sizeof(sem_t *)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (create_shared_memory(&r->memory, params, "unnamed", count * sizeof(sem_t)) < 0) return -1;
    r->storage = (sem_t *)((char *)r->memory.shared + r->memory.shared->extra_offset);
    r->semaphores = calloc(count, sizeof(*r->semaphores));
    if (r->semaphores == NULL) goto fail;
    for (size_t i = 0; i < count; i++) {
        if (sem_init(&r->storage[i], 1, i == 0 ? 1 : 0) < 0) goto fail;
        r->ready++;
        r->semaphores[i] = &r->storage[i];
    }
    init_fork_context(&r->processes, &r->memory.parking, r->semaphores, false);
    return 0;

fail:
    {
        int saved_errno = errno;
        destroy_unnamed_resources(r);
        errno = saved_errno;
        return -1;
    }
}

void destroy_unnamed_resources(UnnamedResources *r) {
    for (size_t i = 0; i < r->ready; i++) sem_destroy(&r->storage[i]);
    r->ready = 0;
    free(r->semaphores);
    r->semaphores = NULL;
    destroy_shared_memory(&r->memory);
}
