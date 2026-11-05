#include "protocol.h"
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void reset_transport(Transport *t) { *t = (Transport){ .requests = (mqd_t)-1, .replies = (mqd_t)-1 }; }
int transport_name(const char *id, char *base, size_t size) {
    if (!*id || strlen(id) > 48) { errno = EINVAL; return -1; }
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) { errno = EINVAL; return -1; }
    int n = snprintf(base, size, "/parking_mq_%s", id);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
static void name_of(const Transport *t, int index, bool queue, char name[128]) {
    if (queue) snprintf(name, 128, "%s_%s", t->base, index == 0 ? "requests" : "replies");
    else snprintf(name, 128, "%s_sem_%d", t->base, index);
}
static int wait_sem(sem_t *sem, long ns) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += ns;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    int rc;
    do { rc = sem_timedwait(sem, &deadline); } while (rc < 0 && errno == EINTR);
    return rc;
}
int transport_create(Transport *t, const char *id) {
    reset_transport(t);
    if (transport_name(id, t->base, sizeof(t->base)) < 0) return -1;
    t->owner = true;
    struct mq_attr attr = { .mq_maxmsg = MQ_CAPACITY, .mq_msgsize = sizeof(Message) };
    for (int i = 0; i < 2; i++) {
        char name[128]; name_of(t, i, true, name);
        mqd_t q = mq_open(name, O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
        if (q == (mqd_t)-1) goto fail;
        if (i == 0) t->requests = q; else t->replies = q;
        t->queues_owned[i] = true;
    }
    for (int i = 0; i < 4; i++) {
        char name[128]; name_of(t, i, false, name);
        sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, i == 1 ? 1U : 0U);
        if (sem == SEM_FAILED) goto fail;
        t->sems[i] = sem; t->sems_owned[i] = true;
    }
    return 0;
fail:
    { int saved = errno; transport_close(t); errno = saved; return -1; }
}
int transport_publish(Transport *t) { return sem_post(t->sems[0]); }
void transport_close(Transport *t) {
    for (int i = 0; i < 2; i++) {
        if (t->queues_owned[i]) {
            char name[128]; name_of(t, i, true, name);
            if (mq_unlink(name) < 0 && errno != ENOENT)
                if (mq_unlink(name) < 0 && errno != ENOENT) report_error("mq_unlink");
        }
    }
    for (int i = 0; i < 4; i++) {
        if (t->sems_owned[i]) {
            char name[128]; name_of(t, i, false, name);
            if (sem_unlink(name) < 0 && errno != ENOENT)
                if (sem_unlink(name) < 0 && errno != ENOENT) report_error("sem_unlink MQ");
        }
        if (t->sems[i]) sem_close(t->sems[i]);
    }
    if (t->requests != (mqd_t)-1) mq_close(t->requests);
    if (t->replies != (mqd_t)-1) mq_close(t->replies);
    reset_transport(t);
}
int transport_connect(Transport *t, const char *id) {
    long long end = monotonic_ns() + 5000000000LL;
    for (;;) {
        reset_transport(t);
        if (transport_name(id, t->base, sizeof(t->base)) < 0) return -1;
        char name[128]; name_of(t, 0, true, name);
        t->requests = mq_open(name, O_RDWR | O_NONBLOCK);
        if (t->requests == (mqd_t)-1) goto retry;
        name_of(t, 1, true, name);
        t->replies = mq_open(name, O_RDWR | O_NONBLOCK);
        if (t->replies == (mqd_t)-1) goto retry;
        for (int i = 0; i < 4; i++) {
            name_of(t, i, false, name);
            t->sems[i] = sem_open(name, 0);
            if (t->sems[i] == SEM_FAILED) { t->sems[i] = NULL; goto retry; }
        }
        if (wait_sem(t->sems[0], 500000000L) < 0) goto retry;
        if (sem_post(t->sems[0]) < 0) goto fail;
        struct mq_attr attr;
        if (mq_getattr(t->requests, &attr) < 0 || attr.mq_msgsize != (long)sizeof(Message) || attr.mq_maxmsg != MQ_CAPACITY ||
            mq_getattr(t->replies, &attr) < 0 || attr.mq_msgsize != (long)sizeof(Message) || attr.mq_maxmsg != MQ_CAPACITY) {
            errno = EPROTO; goto fail;
        }
        return 0;
retry:
        { int saved = errno; transport_close(t); errno = saved; }
        if (errno != ENOENT && errno != ETIMEDOUT) return -1;
        sigset_t pending; sigpending(&pending);
        if (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)) { errno = ECANCELED; return -1; }
        if (monotonic_ns() >= end) { errno = ETIMEDOUT; return -1; }
        sleep_until(monotonic_ns() + 10000000LL);
    }
fail:
    { int saved = errno; transport_close(t); errno = saved; return -1; }
}
int transport_lock(Transport *t) { return wait_sem(t->sems[1], 10000000L); }
int transport_unlock(Transport *t) { return sem_post(t->sems[1]); }
int transport_send(mqd_t q, const Message *m) {
    int rc;
    do { rc = mq_send(q, (const char *)m, sizeof(*m), 0); } while (rc < 0 && errno == EINTR);
    return rc;
}
int transport_receive(mqd_t q, Message *m) {
    ssize_t n;
    do { n = mq_receive(q, (char *)m, sizeof(*m), NULL); } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (n != (ssize_t)sizeof(*m) || m->magic != MQ_MAGIC) { errno = EPROTO; return -1; }
    return 0;
}
bool transport_alive(const Transport *t) {
    char name[128]; name_of(t, 0, true, name);
    mqd_t q = mq_open(name, O_WRONLY | O_NONBLOCK);
    if (q == (mqd_t)-1) return errno != ENOENT;
    mq_close(q); return true;
}
// За один проход возвращаем только своё сообщение, остальные сохраняем в очереди.
int client_poll(MqClient *c) {
    Transport *t = &c->transport;
    if (transport_lock(t) < 0) return errno == ETIMEDOUT ? 0 : -1;
    struct mq_attr attr;
    Message messages[MQ_CAPACITY];
    int count = 0, target = -1, result = 0;
    if (mq_getattr(t->replies, &attr) < 0 || attr.mq_curmsgs > MQ_CAPACITY) result = -1;
    else for (long i = 0; i < attr.mq_curmsgs; i++) {
        Message m;
        if (transport_receive(t->replies, &m) < 0) { result = -1; break; }
        messages[count] = m;
        if (target < 0 && m.pid == c->pid && m.cookie == c->cookie) target = count;
        count++;
    }
    // Возвращаем все чужие сообщения в исходном порядке. Простое вращение до
    // первого совпадения могло бы поменять местами QUEUED и PARKED другого клиента.
    for (int i = 0; i < count; i++) {
        if (i == target) {
            c->last = messages[i];
            if (c->last.type == MQ_STOP) c->stop = true;
            if (c->last.type == MQ_STATUS) { c->status = c->last.car.status; c->has_status = true; }
        } else if (transport_send(t->replies, &messages[i]) < 0) result = -1;
    }
    if (result == 0 && target >= 0) result = 1;
    if (transport_unlock(t) < 0) return -1;
    return result;
}
int client_call(MqClient *c, MessageType type, int sig, Message *reply) {
    Message request = { .magic = MQ_MAGIC, .cookie = c->cookie, .pid = c->pid,
        .id = c->id, .seq = ++c->seq, .type = type, .signal = sig, .car = c->welcome.car };
    long long end = monotonic_ns() + 5000000000LL;
    while (transport_send(c->transport.requests, &request) < 0) {
        if (errno != EAGAIN) return -1;
        if (!transport_alive(&c->transport)) { errno = ECANCELED; return -1; }
        if (monotonic_ns() >= end) { errno = ETIMEDOUT; return -1; }
        sleep_until(monotonic_ns() + 1000000LL);
    }
    if (sem_post(c->transport.sems[2]) < 0) return -1;
    while (monotonic_ns() < end) {
        int rc = client_poll(c);
        if (rc < 0) return -1;
        if (rc > 0 && c->last.seq == request.seq) {
            if (c->last.type == MQ_ERROR) { errno = c->last.error; return -1; }
            if (reply) *reply = c->last;
            return 0;
        }
        if (!transport_alive(&c->transport)) { errno = ECANCELED; return -1; }
        if (wait_sem(c->transport.sems[3], 1000000L) < 0 && errno != ETIMEDOUT) return -1;
    }
    errno = ETIMEDOUT; return -1;
}
int client_connect(MqClient *c, const char *session, int id) {
    *c = (MqClient){ .pid = getpid(), .id = id };
    c->cookie = (uint64_t)monotonic_ns() ^ ((uint64_t)c->pid << 32);
    if (transport_connect(&c->transport, session) < 0) return -1;
    Message m;
    if (client_call(c, MQ_HELLO, 0, &m) < 0) { int saved = errno; client_close(c); errno = saved; return -1; }
    if (m.type != MQ_WELCOME || m.params.tick_ms < 1) { client_close(c); errno = EPROTO; return -1; }
    c->owner_pid = m.owner_pid;
    c->welcome = m;
    return 0;
}
int client_disconnect(MqClient *c) { return client_call(c, MQ_BYE, 0, NULL); }
void client_close(MqClient *c) { transport_close(&c->transport); }
