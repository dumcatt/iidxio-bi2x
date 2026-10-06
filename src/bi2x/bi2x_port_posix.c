#ifndef _WIN32

/* POSIX backend, used to run the stack against a simulated board over a
   pty (see test/). Autodetection is not supported: pass a device path or set
   BI2X_PORT. */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bi2x_log.h"
#include "bi2x_port.h"

struct bi2x_serial {
    int fd;
};

struct bi2x_mutex {
    pthread_mutex_t m;
};

struct bi2x_thread {
    pthread_t th;
    int (*proc)(void *);
    void *ctx;
};

int bi2x_serial_open(struct bi2x_serial **out, const char *port)
{
    struct termios tio;
    struct bi2x_serial *s;
    int fd;

    *out = NULL;

    if (port == NULL || port[0] == '\0') {
        port = getenv("BI2X_PORT");
    }

    if (port == NULL || port[0] == '\0') {
        return -1;
    }

    fd = open(port, O_RDWR | O_NOCTTY);

    if (fd < 0) {
        return -1;
    }

    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tcsetattr(fd, TCSANOW, &tio);
    }

    s = calloc(1, sizeof(*s));

    if (s == NULL) {
        close(fd);
        return -1;
    }

    s->fd = fd;
    *out = s;

    return 0;
}

void bi2x_serial_close(struct bi2x_serial *s)
{
    if (s != NULL) {
        close(s->fd);
        free(s);
    }
}

int bi2x_serial_write(struct bi2x_serial *s, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = write(s->fd, p, len);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }

        p += n;
        len -= (size_t) n;
    }

    return 0;
}

int bi2x_serial_read(
    struct bi2x_serial *s, void *buf, size_t cap, unsigned int timeout_ms)
{
    struct pollfd pfd;
    ssize_t n;
    int r;

    pfd.fd = s->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    r = poll(&pfd, 1, (int) timeout_ms);

    if (r < 0) {
        return errno == EINTR ? 0 : -1;
    }

    if (r == 0) {
        return 0;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (!(pfd.revents & POLLIN)) {
            return -1;
        }
    }

    n = read(s->fd, buf, cap);

    if (n < 0) {
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    }

    return (int) n;
}

int bi2x_serial_set_break(struct bi2x_serial *s, int on)
{
    /* ptys do not support breaks; ignore failures */
    ioctl(s->fd, on ? TIOCSBRK : TIOCCBRK);
    return 0;
}

void bi2x_serial_purge(struct bi2x_serial *s)
{
    tcflush(s->fd, TCIOFLUSH);
}

uint64_t bi2x_time_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

void bi2x_sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

struct bi2x_mutex *bi2x_mutex_create(void)
{
    struct bi2x_mutex *m = calloc(1, sizeof(*m));

    if (m != NULL) {
        pthread_mutex_init(&m->m, NULL);
    }

    return m;
}

void bi2x_mutex_lock(struct bi2x_mutex *m)
{
    pthread_mutex_lock(&m->m);
}

void bi2x_mutex_unlock(struct bi2x_mutex *m)
{
    pthread_mutex_unlock(&m->m);
}

void bi2x_mutex_destroy(struct bi2x_mutex *m)
{
    if (m != NULL) {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}

static void *thread_trampoline(void *param)
{
    struct bi2x_thread *t = param;

    t->proc(t->ctx);

    return NULL;
}

struct bi2x_thread *bi2x_thread_start(
    const struct bi2x_thread_api *api, int (*proc)(void *), void *ctx)
{
    struct bi2x_thread *t = calloc(1, sizeof(*t));

    (void) api;

    if (t == NULL) {
        return NULL;
    }

    t->proc = proc;
    t->ctx = ctx;

    if (pthread_create(&t->th, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }

    return t;
}

void bi2x_thread_join(struct bi2x_thread *t)
{
    if (t != NULL) {
        pthread_join(t->th, NULL);
        free(t);
    }
}

void bi2x_self_dir(char *out, size_t cap)
{
    if (cap > 0) {
        out[0] = '\0';
    }
}

#endif
