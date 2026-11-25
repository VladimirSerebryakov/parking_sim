#include "fork_sync.h"
#include "io.h"
#include <errno.h>
#include <unistd.h>

void init_fork_context(ForkContext *ctx, const Parking *parking, sem_t **semaphores, bool named) {
    *ctx = (ForkContext){ .parking = *parking, .state_lock = semaphores[0],
        .dispatcher_wake = semaphores[1], .sim_end = semaphores[2],
        .granted = semaphores + 3, .named_semaphores = named };
}

void sem_wait_retry(sem_t *sem) {
    int rc;
    do { rc = sem_wait(sem); } while (rc < 0 && errno == EINTR);
    if (rc < 0) { report_error("sem_wait"); _exit(2); }
}

void sem_post_checked(sem_t *sem) {
    if (sem_post(sem) < 0) { report_error("sem_post"); _exit(2); }
}

void state_lock(ForkContext *ctx) { sem_wait_retry(ctx->state_lock); }
void state_unlock(ForkContext *ctx) { sem_post_checked(ctx->state_lock); }

int close_child_semaphores(ForkContext *ctx) {
    if (!ctx->named_semaphores) return 0;
    int result = 0;
    if (sem_close(ctx->state_lock) < 0) result = -1;
    if (sem_close(ctx->dispatcher_wake) < 0) result = -1;
    if (sem_close(ctx->sim_end) < 0) result = -1;
    for (int i = 0; i < ctx->parking.state->total_cars; i++)
        if (sem_close(ctx->granted[i]) < 0) result = -1;
    if (result < 0) report_error("sem_close в дочернем процессе");
    return result;
}
