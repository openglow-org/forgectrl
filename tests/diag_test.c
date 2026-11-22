/*
 * diag_test.c - host unit test for the cooling diagnostic's coolant reads
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * diag.c's air-assist calibration window, against a fake sysfs tree and a
 * clock that never sleeps. A count that is no temperature (coolant.h: 0,
 * the converter's 1023 rail and past it, a count under 0 C) is no reading
 * here as everywhere: the window takes no sample of it (an open sensor's
 * rail would read as a step), and takes a reading.
 *
 * And the daemon's stop: a run in flight is asked to end, and the stop
 * returns once it has.
 */
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>

static int real_pause;                  /* each sleep is 1 ms of real time (the stop's case) */
static int fake_usleep(useconds_t us)
{
    (void)us;
    if (real_pause) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
static unsigned fake_sleep(unsigned s) { (void)s; return 0; }
#define usleep fake_usleep
#define sleep  fake_sleep

#include "../src/diag.c"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* --- the daemon around the diagnostic, which these cases never reach -- */

/* The order of the controller's stop and the engine's take, by turn. */
static int turn, stop_turn, take_turn;
static int engine_busy;                 /* the engine refuses the take (-1) */
static int starts;                      /* controller hand-backs */
int  cool_diag_can_take(void) { return engine_busy ? -1 : 0; }
int  cool_diag_take(void) { take_turn = ++turn; return engine_busy ? -1 : 0; }
void cool_diag_release(void) { }
void cool_diag_pump(int on) { (void)on; }
void cool_diag_heater_pct(double pct) { (void)pct; }
void cool_diag_fans_idle(void) { }
void cool_diag_aa(long duty) { (void)duty; }
int  lease_take(const char *owner, lease_kind_t kind, const char *under, char *why, size_t len)
{
    (void)owner; (void)kind; (void)under; (void)why; (void)len;
    return 0;
}
void lease_release(const char *owner) { (void)owner; }
int  super_takeover_begin(void) { stop_turn = ++turn; return 0; }
void super_takeover_end(void) { starts++; }
int  wizdark_lease_owner(char *buf, size_t len) { (void)buf; (void)len; return 0; }
int  machine_is_idle(void) { return 1; }
void fflog(int prio, const char *fmt, ...) { (void)prio; (void)fmt; }

/* A run's thread as runner_aa ends it: once the abort is asked for, it
 * stands down and the run is over. */
static void *stand_down(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        int asked = abort_req;
        if (asked)
            st_running = 0;
        pthread_mutex_unlock(&mu);
        if (asked)
            return NULL;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

/* status.c's conversion, which is coolant.h's. */
double coolant_degc(long raw)
{
    return coolant_c_of_counts((double)raw);
}

/* --- the tree --------------------------------------------------------- */

#define ROOT "diag-test/"

static void put_long(const char *attr, long v)
{
    char path[160];
    snprintf(path, sizeof(path), ROOT "%s", attr);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld\n", v);
        fclose(f);
    }
}

static void coolant(long down, long up)
{
    put_long("pic/water_temp_1", down);
    put_long("pic/water_temp_2", up);
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    setenv("FORGECTRL_HOST_TEST", "1", 1);      /* the test hooks' gate (testhook.h) */
    setenv("GF_SYSFS_ROOT", ROOT, 1);
    mkdir(ROOT, 0755);
    mkdir(ROOT "pic", 0755);

    printf("the calibration's window: no sample on a count that is no temperature\n");
    struct aa_win w;
    coolant(1023, 694);
    CHECK(aa_window(&w) == -1 && w.n == 0, "the downstream channel at the rail: no sample, the window refused");
    coolant(694, 990);
    CHECK(aa_window(&w) == -1 && w.n == 0, "the upstream channel under 0 C: no sample, the window refused");
    coolant(700, 694);
    CHECK(aa_window(&w) == 0 && w.n == AA_N && w.m1 == 700.0 && w.m2 == 694.0,
          "both channels reading: every sample taken");

    printf("the daemon's stop ends a run in flight and waits for it\n");
    {
        /* A run in flight, its thread standing down once asked to. */
        pthread_mutex_lock(&mu);
        st_running = 1;
        abort_req = 0;
        pthread_mutex_unlock(&mu);
        pthread_t t;
        pthread_create(&t, NULL, stand_down, NULL);
        real_pause = 1;
        diag_shutdown();
        real_pause = 0;
        int ended = !diag_running();
        diag_abort();                   /* a thread that was never asked is let go */
        pthread_mutex_lock(&mu);
        abort_req = 1;
        pthread_mutex_unlock(&mu);
        pthread_join(t, NULL);
        CHECK(ended, "the run was asked to end, and the stop returned once it had");
    }

    printf("the air-assist run stops the controller before the engine hands the hardware over\n");
    {
        pthread_mutex_lock(&mu);
        st_running = 1;
        abort_req = 1;                  /* the run stands down at its first look */
        pthread_mutex_unlock(&mu);
        turn = stop_turn = take_turn = 0;
        runner_aa(NULL);
        CHECK(stop_turn == 1 && take_turn == 2, "the controller's stop first, then the engine's take");
        /* The engine busy at the take (it went busy after the start's
         * check): the controller is handed back, and nothing runs. */
        pthread_mutex_lock(&mu);
        st_running = 1;
        abort_req = 0;
        pthread_mutex_unlock(&mu);
        engine_busy = 1;
        starts = 0;
        runner_aa(NULL);
        CHECK(starts == 1 && !diag_running(), "a take the engine refuses: the controller handed back, the run over");
        /* A busy engine refuses the start itself, with no thread. */
        char why[64] = "";
        CHECK(diag_start("aa-offset-calibrate", why, sizeof(why)) == -2 && !diag_running(),
              "a busy engine: the start refused");
        engine_busy = 0;
    }

    unlink(ROOT "pic/water_temp_1");
    unlink(ROOT "pic/water_temp_2");
    rmdir(ROOT "pic");
    rmdir(ROOT);
    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the cooling diagnostic takes no coolant count that is no temperature\n", failures);
    return failures ? 1 : 0;
}
