/*
 * button_test.c - host unit test for the button's hold and its wait
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * button.c against a fake clock and a fake switch device:
 *
 *   A. the ten-second hold (the account reset at the daemon's start) is ten
 *      seconds on the clock, from wherever in a second it begins: a hold
 *      let go at 9.5 s is no hold, one held 10 s is
 *   B. the waiting thread is joined by one caller alone: a start that comes
 *      while a cancel is joining it finds nothing to join
 *   C. the wait's timeout (the advisories' press) is its seconds on the
 *      clock, from wherever in a second it begins
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int fake_clock_gettime(clockid_t id, struct timespec *ts);
int fake_nanosleep(const struct timespec *req, struct timespec *rem);
int fake_open(const char *path, int flags, ...);
int fake_ioctl(int fd, unsigned long req, ...);
int fake_pthread_join(pthread_t t, void **ret);
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
#define open fake_open
#define ioctl fake_ioctl
#define pthread_join fake_pthread_join

#include "../src/button.c"

#undef clock_gettime
#undef nanosleep
#undef open
#undef ioctl
#undef pthread_join

static double fake_now = 1000.7;        /* a start inside a second */
static double release_at = -1.0;        /* the button lets go here (-1: held) */
static int button_down = 1;
static int real_pause;                  /* each fake sleep also sleeps 1 ms */

int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    return 0;
}

int fake_nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    fake_now += (double)req->tv_sec + req->tv_nsec / 1e9;
    if (real_pause) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int fake_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int fake_ioctl(int fd, unsigned long req, ...)
{
    (void)fd;
    (void)req;
    va_list ap;
    va_start(ap, req);
    uint8_t *sw = va_arg(ap, uint8_t *);
    va_end(ap);
    int down = button_down && (release_at < 0 || fake_now < release_at);
    sw[0] = down ? (uint8_t)(1u << SW_BIT_BUTTON) : 0;
    sw[1] = 0;
    return 0;
}

/* The joins, in order, and the one interleaving case B makes: inside the
 * cancel's join, once the waiting thread has ended, a start comes. */
static pthread_t joined[8];
static int njoined;
static int start_in_join;
static int start_rc = -9;

int fake_pthread_join(pthread_t t, void **ret)
{
    int again = 0;
    for (int i = 0; i < njoined && i < 8; i++)
        again |= pthread_equal(joined[i], t);
    if (njoined < 8)
        joined[njoined] = t;
    njoined++;
    if (again)
        return ESRCH;           /* a second join of one thread: noted, never made */
    if (start_in_join) {
        start_in_join = 0;
        while (strcmp(button_state(), "waiting") == 0) {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
        start_rc = button_wait_start(1000);
    }
    return pthread_join(t, ret);
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    printf("A. the ten-second hold is ten seconds on the clock\n");
    double t0 = fake_now;
    release_at = t0 + 9.5;
    CHECK(button_held_for(10) == 0, "a hold let go at 9.5 s is no ten-second hold");
    fake_now = 2000.7;
    t0 = fake_now;
    release_at = -1.0;
    CHECK(button_held_for(10) == 1 && fake_now - t0 > 9.99, "one held for 10 s is");

    printf("B. the waiting thread is joined by one caller alone\n");
    button_down = 0;
    real_pause = 1;
    CHECK(button_wait_start(1000) == 0, "a wait starts");
    pthread_t first = th;
    njoined = 0;
    start_in_join = 1;
    button_wait_cancel();
    int twice = 0;
    for (int i = 0; i < njoined && i < 8; i++)
        for (int j = i + 1; j < njoined && j < 8; j++)
            twice |= pthread_equal(joined[i], joined[j]);
    CHECK(start_rc == 0 && njoined == 1 && pthread_equal(joined[0], first) && !twice,
          "a start during the cancel's join joins nothing: the old thread is joined once, by the cancel");
    button_wait_cancel();               /* the start's own wait */
    real_pause = 0;

    printf("C. the wait's timeout is its seconds on the clock\n");
    fake_now = 3000.7;
    t0 = fake_now;
    real_pause = 1;
    CHECK(button_wait_start(2) == 0, "a two-second wait starts, the button up");
    for (int i = 0; i < 5000 && strcmp(button_state(), "waiting") == 0; i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    CHECK(strcmp(button_state(), "timeout") == 0 && fake_now - t0 > 1.99 && fake_now - t0 < 2.1,
          "it times out at 2 s, from wherever in a second it began, not before");
    real_pause = 0;

    printf(failures ? "button_test: %d FAILED\n" : "button_test: all ok\n", failures);
    return failures ? 1 : 0;
}
