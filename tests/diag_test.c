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
 */
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>

static int fake_usleep(useconds_t us) { (void)us; return 0; }
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

int  cool_diag_take(void) { return 0; }
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
int  super_takeover_begin(void) { return 0; }
void super_takeover_end(void) { }
int  wizdark_lease_owner(char *buf, size_t len) { (void)buf; (void)len; return 0; }
int  machine_is_idle(void) { return 1; }
void fflog(int prio, const char *fmt, ...) { (void)prio; (void)fmt; }

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

    unlink(ROOT "pic/water_temp_1");
    unlink(ROOT "pic/water_temp_2");
    rmdir(ROOT "pic");
    rmdir(ROOT);
    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the cooling diagnostic takes no coolant count that is no temperature\n", failures);
    return failures ? 1 : 0;
}
