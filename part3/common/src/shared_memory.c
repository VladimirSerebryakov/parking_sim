#include "shared_memory.h"
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Проверка размера и выравнивание массивов общей модели.
static int reserve_array(size_t *size, size_t count, size_t element_size,
                         size_t alignment, size_t *offset) {
    size_t padding = (alignment - *size % alignment) % alignment;
    if (*size > SIZE_MAX - padding) return -1;
    *size += padding;
    *offset = *size;
    if (count > (SIZE_MAX - *size) / element_size) return -1;
    *size += count * element_size;
    return 0;
}

static int create_memory(SharedHandle *h, const SimParams *params,
                         const char *variant, const char *session_name, size_t extra_bytes) {
    *h = (SharedHandle){ .parking = { .log_fd = -1 } };
    int total_spots = params->spots_count[0] + params->spots_count[1] + params->spots_count[2];
    int total_cars = params->cars_count[0] + params->cars_count[1] + params->cars_count[2];
    size_t size = sizeof(SharedMemory), spots, cars, inbox, queue, extra;
    if (reserve_array(&size, total_spots, sizeof(ParkingSpot), _Alignof(ParkingSpot), &spots) ||
        reserve_array(&size, total_cars, sizeof(Car), _Alignof(Car), &cars) ||
        reserve_array(&size, total_cars, sizeof(int), _Alignof(int), &inbox) ||
        reserve_array(&size, params->queue_capacity, sizeof(int), _Alignof(int), &queue) ||
        reserve_array(&size, extra_bytes, 1, _Alignof(max_align_t), &extra) ||
        size > LLONG_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    // O_EXCL исключает подключение к чужому объекту даже при совпадении имени.
    int fd = -1;
    for (int attempt = 0; attempt < 16; attempt++) {
        if (session_name) {
            if (strlen(session_name) >= sizeof(h->name)) { errno = ENAMETOOLONG; return -1; }
            strcpy(h->name, session_name);
        } else snprintf(h->name, sizeof(h->name), "/parking_%s_%ld_%lld_%d",
                        variant, (long)getpid(), monotonic_ns(), attempt);
        fd = shm_open(h->name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0 || errno != EEXIST || session_name) break;
    }
    if (fd < 0) return -1;
    h->name_owned = true;
    int rc = ftruncate(fd, (off_t)size);
    // ftruncate задаёт размер, но не резервирует место: без этого инициализация
    // слишком большой области могла бы получить SIGBUS вместо обычной ошибки ENOSPC.
    if (rc == 0) {
        int allocation_error = posix_fallocate(fd, 0, (off_t)size);
        if (allocation_error != 0) { errno = allocation_error; rc = -1; }
    }
    void *mapping = MAP_FAILED;
    if (rc == 0) mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved_errno = errno;
    close(fd);
    if (mapping == MAP_FAILED) {
        unlink_shared_memory(h);
        errno = saved_errno;
        return -1;
    }
    h->shared = mapping;
    if (!session_name && unlink_shared_memory(h) < 0) {
        saved_errno = errno;
        munmap(mapping, size);
        h->shared = NULL;
        unlink_shared_memory(h);
        errno = saved_errno;
        return -1;
    }
    h->size = size;
    h->shared->spots_offset = spots;
    h->shared->cars_offset = cars;
    h->shared->inbox_offset = inbox;
    h->shared->queue_offset = queue;
    h->shared->extra_offset = extra;
    char *base = mapping;
    h->parking.state = &h->shared->state;
    h->parking.spots = (ParkingSpot *)(base + spots);
    h->parking.cars = (Car *)(base + cars);
    h->parking.inbox_ids = (int *)(base + inbox);
    h->parking.queue_ids = (int *)(base + queue);
    ParkingState *s = h->parking.state;
    s->params = *params;
    s->clock = (SimClock){ .start_ns = monotonic_ns(), .tick_ms = params->tick_ms };
    s->total_spots = total_spots;
    s->total_cars = total_cars;
    s->inbox.capacity = total_cars;
    s->queue.capacity = params->queue_capacity;
    int idx = 0;
    for (int type = 0; type < 3; type++) {
        for (int j = 0; j < params->spots_count[type]; j++, idx++)
            h->parking.spots[idx] = (ParkingSpot){ .id = idx + 1, .type = (CarType)type, .car_idx = -1 };
    }
    h->parking.log_fd = open(params->log_path, O_WRONLY | O_CREAT | O_TRUNC | (session_name ? O_APPEND : 0), 0644);
    if (h->parking.log_fd < 0) goto fail;
    return 0;

fail:
    saved_errno = errno;
    destroy_shared_memory(h);
    errno = saved_errno;
    return -1;
}

int create_shared_memory(SharedHandle *h, const SimParams *params,
                         const char *variant, size_t extra_bytes) {
    return create_memory(h, params, variant, NULL, extra_bytes);
}

int create_session_memory(SharedHandle *h, const SimParams *params, const char *name, size_t extra_bytes) {
    return create_memory(h, params, NULL, name, extra_bytes);
}

int unlink_shared_memory(SharedHandle *h) {
    if (!h->name_owned) return 0;
    if (shm_unlink(h->name) < 0 && errno != ENOENT) return -1;
    h->name_owned = false;
    return 0;
}

void destroy_shared_memory(SharedHandle *h) {
    if (unlink_shared_memory(h) < 0) report_error("shm_unlink при очистке");
    if (h->parking.log_fd >= 0) close(h->parking.log_fd);
    if (h->shared != NULL) {
        munmap(h->shared, h->size);
    }
    h->shared = NULL;
    h->parking.log_fd = -1;
}

// Проверяем диапазон и выравнивание до построения локальных указателей.
static bool valid_region(size_t size, size_t offset, size_t count, size_t element, size_t alignment) {
    return offset >= sizeof(SharedMemory) && offset <= size && offset % alignment == 0 &&
           count <= (size - offset) / element;
}

int attach_shared_memory(SharedHandle *h, const char *name) {
    *h = (SharedHandle){ .parking = { .log_fd = -1 } };
    if (strlen(name) >= sizeof(h->name)) { errno = ENAMETOOLONG; return -1; }
    strcpy(h->name, name);
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(SharedMemory) || (uintmax_t)st.st_size > SIZE_MAX) {
        int saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    h->size = (size_t)st.st_size;
    void *map = mmap(NULL, h->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (map == MAP_FAILED) { errno = saved; return -1; }
    h->shared = map;
    SharedMemory *m = map;
    ParkingState *s = &m->state;
    if (s->total_cars < 0 || s->total_spots < 0 || s->queue.capacity < 0 ||
        s->clock.tick_ms < 1 || s->clock.tick_ms != s->params.tick_ms ||
        s->inbox.capacity != s->total_cars || s->queue.capacity != s->params.queue_capacity ||
        !memchr(s->params.log_path, 0, sizeof(s->params.log_path)) ||
        !valid_region(h->size, m->spots_offset, s->total_spots, sizeof(ParkingSpot), _Alignof(ParkingSpot)) ||
        !valid_region(h->size, m->cars_offset, s->total_cars, sizeof(Car), _Alignof(Car)) ||
        !valid_region(h->size, m->inbox_offset, s->total_cars, sizeof(int), _Alignof(int)) ||
        !valid_region(h->size, m->queue_offset, s->queue.capacity, sizeof(int), _Alignof(int)) ||
        m->extra_offset > h->size || m->extra_offset < sizeof(SharedMemory) ||
        m->extra_offset % _Alignof(max_align_t) != 0) { errno = EINVAL; goto fail_attach; }
    char *base = map;
    h->parking.state = s;
    h->parking.spots = (ParkingSpot *)(base + m->spots_offset);
    h->parking.cars = (Car *)(base + m->cars_offset);
    h->parking.inbox_ids = (int *)(base + m->inbox_offset);
    h->parking.queue_ids = (int *)(base + m->queue_offset);
    h->parking.log_fd = open(s->params.log_path, O_WRONLY | O_APPEND);
    if (h->parking.log_fd < 0) goto fail_attach;
    return 0;

fail_attach:
    saved = errno;
    destroy_shared_memory(h);
    errno = saved;
    return -1;
}
