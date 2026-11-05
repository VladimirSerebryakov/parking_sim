#include "named_resources.h"
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unlink_retry(const char *name) {
    int rc;
    do { rc = sem_unlink(name); } while (rc < 0 && errno == EINTR);
    return rc;
}

int create_named_resources(NamedResources *r, const SimParams *params) {
    *r = (NamedResources){ .memory = { .parking = { .log_fd = -1 } } };
    size_t count = (size_t)params->cars_count[0] + params->cars_count[1] + params->cars_count[2] + 3;
    if (count > SIZE_MAX / sizeof(sem_t *)) { errno = EOVERFLOW; return -1; }
    if (create_shared_memory(&r->memory, params, "named", 0) < 0) return -1;
    r->semaphores = calloc(count, sizeof(*r->semaphores));
    if (r->semaphores == NULL) goto fail;
    for (size_t i = 0; i < count; i++) {
        char name[sizeof(r->pending_name)];
        // Номера 0/1/2 — state_lock/dispatcher_wake/sim_end; остальные — granted[i - 3].
        snprintf(name, sizeof(name), "%s_sem_%zu", r->memory.name, i);
        sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, i == 0 ? 1U : 0U);
        if (sem == SEM_FAILED) goto fail;
        r->semaphores[r->opened++] = sem;
        // Имя записывается для отката только после успешного O_EXCL: чужое имя не удаляем.
        strcpy(r->pending_name, name);
        if (unlink_retry(name) < 0) goto fail;
        r->pending_name[0] = '\0';
    }
    // fork наследует открытые объекты даже после sem_unlink; адреса не помещаем в общую модель.
    init_fork_context(&r->processes, &r->memory.parking, r->semaphores, true);
    return 0;

fail:
    {
        int saved_errno = errno;
        destroy_named_resources(r);
        errno = saved_errno;
        return -1;
    }
}

void destroy_named_resources(NamedResources *r) {
    if (r->pending_name[0] != '\0') {
        if (unlink_retry(r->pending_name) < 0 && errno != ENOENT) report_error("sem_unlink при очистке");
        r->pending_name[0] = '\0';
    }
    for (size_t i = 0; i < r->opened; i++)
        if (sem_close(r->semaphores[i]) < 0) report_error("sem_close при очистке");
    r->opened = 0;
    free(r->semaphores);
    r->semaphores = NULL;
    destroy_shared_memory(&r->memory);
}
