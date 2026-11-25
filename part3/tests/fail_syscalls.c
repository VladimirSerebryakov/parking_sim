// Подмена только для тестов отката: LD_PRELOAD, PARKING_FAIL=операция:номер_вызова.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <spawn.h>
#include <mqueue.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static pid_t fork_parent; // Исходный родитель для теста гибели владельца state_lock

static void trace(const char *operation, const char *name, int result) {
    const char *path = getenv("PARKING_TRACE");
    if (path == NULL) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    char line[256];
    int len = snprintf(line, sizeof(line), "%s %ld %s %d\n", operation, (long)getpid(), name, result);
    ssize_t written = write(fd, line, (size_t)len);
    (void)written;
    close(fd);
}

static int fail_at(const char *operation, int *count) {
    const char *requested = getenv("PARKING_FAIL");
    char current[64];
    snprintf(current, sizeof(current), "%s:%d", operation, ++*count);
    if (requested && strcmp(requested, current) == 0) { errno = EAGAIN; return 1; }
    return 0;
}

int shm_open(const char *name, int flags, mode_t mode) {
    static int count;
    trace("shm_open", name, 0);
    if (fail_at("shm_open", &count)) return -1;
    int (*original)(const char *, int, mode_t) = dlsym(RTLD_NEXT, "shm_open");
    return original(name, flags, mode);
}

int ftruncate(int fd, off_t size) {
    static int count;
    if (fail_at("ftruncate", &count)) return -1;
    int (*original)(int, off_t) = dlsym(RTLD_NEXT, "ftruncate");
    return original(fd, size);
}

int posix_fallocate(int fd, off_t offset, off_t size) {
    static int count;
    if (fail_at("posix_fallocate", &count)) return EAGAIN;
    int (*original)(int, off_t, off_t) = dlsym(RTLD_NEXT, "posix_fallocate");
    return original(fd, offset, size);
}

void *mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    static int count;
    if ((flags & MAP_SHARED) && fd >= 0 && fail_at("mmap", &count)) return MAP_FAILED;
    void *(*original)(void *, size_t, int, int, int, off_t) = dlsym(RTLD_NEXT, "mmap");
    return original(address, size, prot, flags, fd, offset);
}

int sem_init(sem_t *sem, int pshared, unsigned int value) {
    static int count;
    trace("sem_init", "-", (int)value);
    if (fail_at("sem_init", &count)) return -1;
    int (*original)(sem_t *, int, unsigned int) = dlsym(RTLD_NEXT, "sem_init");
    return original(sem, pshared, value);
}

sem_t *sem_open(const char *name, int flags, ...) {
    static int count;
    mode_t mode = 0;
    unsigned int value = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        value = va_arg(args, unsigned int);
        va_end(args);
    }
    sem_t *(*original)(const char *, int, ...) = dlsym(RTLD_NEXT, "sem_open");
    int (*original_close)(sem_t *) = dlsym(RTLD_NEXT, "sem_close");
    const char *fault = getenv("PARKING_FAIL");
    if (fault && strcmp(fault, "sem_collision:1") == 0 && count == 0) {
        sem_t *foreign = original(name, O_CREAT | O_EXCL, 0600, 0U);
        if (foreign != SEM_FAILED) { original_close(foreign); trace("collision", name, 0); }
    }
    if (fail_at("sem_open", &count)) return SEM_FAILED;
    // Успешный тестовый вызов должен иметь эксклюзивное создание, права и значение по протоколу.
    unsigned int initial = getenv("PARKING_SEM_ZERO") ? 0U : (count == 1 ? 1U : 0U);
    if (getenv("PARKING_MQ_MODE")) {
        size_t len = strlen(name);
        initial = len >= 6 && strcmp(name + len - 6, "_sem_1") == 0 ? 1U : 0U;
    }
    if ((flags & O_CREAT) && ((flags & O_EXCL) == 0 || mode != 0600 || value != initial)) {
        errno = EINVAL; return SEM_FAILED;
    }
    sem_t *sem = original(name, flags, mode, value);
    int saved_errno = errno;
    trace("sem_open", name, sem == SEM_FAILED ? -1 : 0);
    errno = saved_errno;
    return sem;
}

int sem_unlink(const char *name) {
    static int count;
    if (fail_at("sem_unlink", &count)) return -1;
    int (*original)(const char *) = dlsym(RTLD_NEXT, "sem_unlink");
    int rc = original(name);
    int saved_errno = errno;
    trace("sem_unlink", name, rc);
    errno = saved_errno;
    return rc;
}

int sem_close(sem_t *sem) {
    int (*original)(sem_t *) = dlsym(RTLD_NEXT, "sem_close");
    int rc = original(sem);
    int saved_errno = errno;
    trace("sem_close", "-", rc);
    errno = saved_errno;
    return rc;
}

pid_t fork(void) {
    static int count;
    if (fail_at("fork", &count)) return -1;
    fork_parent = getpid();
    pid_t (*original)(void) = dlsym(RTLD_NEXT, "fork");
    return original();
}

int sem_wait(sem_t *sem) {
    int (*original)(sem_t *) = dlsym(RTLD_NEXT, "sem_wait");
    int rc = original(sem);
    const char *fault = getenv("PARKING_FAIL");
    if (rc == 0 && fork_parent && getpid() != fork_parent && fault &&
        strcmp(fault, "crash_lock:1") == 0) _exit(77);
    return rc;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    static int count;
    if (fail_at("posix_spawn", &count)) return EAGAIN;
    int (*original)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                    const posix_spawnattr_t *, char *const [], char *const []) = dlsym(RTLD_NEXT, "posix_spawn");
    return original(pid, path, actions, attributes, argv, envp);
}

mqd_t mq_open(const char *name, int flags, ...) {
    static int count;
    mode_t mode = 0; struct mq_attr *attr = NULL;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        mode = va_arg(args, mode_t); attr = va_arg(args, struct mq_attr *); va_end(args);
    }
    if (fail_at("mq_open", &count)) return (mqd_t)-1;
    mqd_t (*original)(const char *, int, ...) = dlsym(RTLD_NEXT, "mq_open");
    mqd_t q = original(name, flags, mode, attr);
    int saved = errno;
    trace("mq_open", name, q == (mqd_t)-1 ? -1 : 0);
    errno = saved;
    return q;
}
int mq_unlink(const char *name) {
    static int count;
    if (fail_at("mq_unlink", &count)) return -1;
    int (*original)(const char *) = dlsym(RTLD_NEXT, "mq_unlink");
    int rc = original(name); int saved = errno;
    trace("mq_unlink", name, rc); errno = saved; return rc;
}
int mq_send(mqd_t q, const char *message, size_t size, unsigned int priority) {
    static int count;
    if (fail_at("mq_send", &count)) return -1;
    int (*original)(mqd_t, const char *, size_t, unsigned int) = dlsym(RTLD_NEXT, "mq_send");
    return original(q, message, size, priority);
}
ssize_t mq_receive(mqd_t q, char *message, size_t size, unsigned int *priority) {
    static int count;
    if (fail_at("mq_receive", &count)) { errno = EINTR; return -1; }
    ssize_t (*original)(mqd_t, char *, size_t, unsigned int *) = dlsym(RTLD_NEXT, "mq_receive");
    return original(q, message, size, priority);
}
