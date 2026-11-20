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
 *   D. the wait's judge (the Setup page's reservation): a press held from
 *      before it came in force counts nothing, the next press of 0.25 s
 *      does; the judge ending the wait ends it cancelled; a press it
 *      refuses is no press
 *   E. the wait's end hook runs once for every wait, at its timeout and at
 *      its cancel, while the state still reads "waiting"
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
static int (*level_at)(double now);     /* the button's level on the clock, when set */

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
    int down = level_at ? level_at(fake_now) : button_down && (release_at < 0 || fake_now < release_at);
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

/* Case D's judge and levels, on the fake clock from d_t0. */
static double d_t0;
static double judge_from = 1.0;         /* in force from here */
static double judge_until = 99.0;       /* the wait ends here */
static int judge_at_press = 1;

static int judge(int at_press)
{
    double t = fake_now - d_t0;
    if (at_press)
        return judge_at_press;
    return t >= judge_until ? -1 : t < judge_from ? 0 : 1;
}

/* Up, then held from 0.3 s across the judge's first second to 1.5 s, up,
 * then a press of 0.25 s at 2 s. */
static int level_d1(double now)
{
    double t = now - d_t0;
    return (t >= 0.3 && t < 1.5) || (t >= 2.0 && t < 2.25);
}

static int level_up(double now)
{
    (void)now;
    return 0;
}

/* Up, then a press of 0.25 s at 0.5 s. */
static int level_d3(double now)
{
    double t = now - d_t0;
    return t >= 0.5 && t < 0.75;
}

/* Case E's hook: how often it ran, and whether the state still read
 * "waiting" when it did. */
static int ended_n, ended_state_waiting;
static void ended(void)
{
    ended_n++;
    ended_state_waiting += strcmp(button_state(), "waiting") == 0;
}

static void wait_over(void)
{
    for (int i = 0; i < 20000 && strcmp(button_state(), "waiting") == 0; i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

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

    printf("D. the wait's judge: a press counts from a release read in force, and a press it refuses is no press\n");
    button_wait_judge = judge;
    real_pause = 1;
    fake_now = 4000.7;
    d_t0 = fake_now;
    level_at = level_d1;
    judge_at_press = 1;
    CHECK(button_wait_start(10) == 0, "a wait starts, its judge not in force for the first second");
    wait_over();
    CHECK(strcmp(button_state(), "pressed") == 0 && fake_now - d_t0 > 1.99 && fake_now - d_t0 < 2.3,
          "a press held from before the judge came in force counts nothing; the next press of 0.25 s counts");
    button_take_pressed();
    fake_now = 5000.7;
    d_t0 = fake_now;
    level_at = level_up;
    judge_from = 0.0;
    judge_until = 2.0;
    CHECK(button_wait_start(10) == 0, "a wait starts, the button up");
    wait_over();
    CHECK(strcmp(button_state(), "cancelled") == 0 && fake_now - d_t0 < 3.0,
          "the judge ending the wait (at 2 s) ends it cancelled, not at its timeout");
    fake_now = 6000.7;
    d_t0 = fake_now;
    level_at = level_d3;
    judge_until = 99.0;
    judge_at_press = -1;
    CHECK(button_wait_start(10) == 0, "a wait starts, its judge refusing at the press");
    wait_over();
    CHECK(strcmp(button_state(), "cancelled") == 0 && fake_now - d_t0 < 1.0,
          "a press the judge refuses ends the wait cancelled, never pressed");
    real_pause = 0;
    level_at = NULL;
    button_wait_judge = NULL;

    printf("E. the wait's end hook runs once for every wait, however it ends\n");
    button_wait_ended = ended;
    real_pause = 1;
    fake_now = 7000.7;
    d_t0 = fake_now;
    button_down = 0;
    CHECK(button_wait_start(1) == 0, "a one-second wait starts, the button up");
    wait_over();
    CHECK(strcmp(button_state(), "timeout") == 0 && ended_n == 1 && ended_state_waiting == 1,
          "its timeout runs the hook once, before the state leaves waiting");
    CHECK(button_wait_start(10) == 0, "a wait starts");
    njoined = 0;                        /* case B's record of joins: a thread id may come again */
    button_wait_cancel();
    CHECK(strcmp(button_state(), "cancelled") == 0 && ended_n == 2, "its cancel runs the hook once");
    real_pause = 0;
    button_wait_ended = NULL;

    printf(failures ? "button_test: %d FAILED\n" : "button_test: all ok\n", failures);
    return failures ? 1 : 0;
}
