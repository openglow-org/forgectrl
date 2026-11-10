/*
 * flowwatch_test.c - host unit test for the coolant flow watch
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The arithmetic (flowcalc.c), then the watch (flowwatch.c) on a model of the
 * flow board: two first-order sensors behind one heater, a flowing loop
 * shedding the heat a stopped one keeps (the bench reference's shape, not
 * its numbers), the run fans raising what flowing costs, and 1 s means.
 * Every case is a rule:
 *
 *   A. the PI holds the step and keeps its integral at the rails; the ring's
 *      means; the fall detector's means
 *   B. a start: a rest with the heater off, then the hold; the idle
 *      reference; an idle summary every 10 minutes
 *   C. a pump that quits at idle: the fall detector names it
 *   D. a job: the floor and the fall detector name a stop under it, and a
 *      summary at its end; a flowing job crosses neither
 *   E. the engine takes the heater back: off at once, a full rest after
 *   F. cool_flow_watch 0 (the default): nothing heated; on again after a rest
 *   G. no readings: the heater off within 2 s; a hold pinned at its most
 *      stops the watch for the start, and the setting does not wake it;
 *      downstream at the 45 C rail cuts the heater, 0.5 C under it does not
 *   H. a hold that settles at no duty gives no idle reference: the floor
 *      judges nothing under the job after it, and no line divides by it
 */
#include "../src/flowwatch.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static char logbuf[32768];
static int nan_lines;                   /* every line logged, in every case: none prints a number never measured */
static void cap_log(int prio, const char *fmt, ...)
{
    (void)prio;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (strstr(line, "nan") || strstr(line, "inf"))
        nan_lines++;
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof(logbuf))
        snprintf(logbuf + n, sizeof(logbuf) - n, "%s\n", line);
    if (getenv("VERBOSE"))
        printf("    log: %s\n", line);
}
static int log_has(const char *s) { return strstr(logbuf, s) != NULL; }

static void test_calc(void)
{
    printf("A. the PI and its settings, the ring, the falls\n");
    flowcalc_cfg_t c = flowcalc_cfg_default();
    flowcalc_pi_t pi;
    flowcalc_pi_reset(&pi);
    double d = 0.0;
    for (int i = 0; i < 200; i++)
        d = flowcalc_pi_step(&pi, &c, 0.0, 0.5);       /* nothing rises: the rail */
    CHECK(d == c.duty_max && pi.integ <= c.duty_max, "pinned at its most, the integral held there");
    d = flowcalc_pi_step(&pi, &c, 2.0, 0.5);            /* over the step: it comes off at once */
    CHECK(d < c.duty_max, "over the step, the duty comes off the rail at once");
    c.duty_min = 2.0;
    flowcalc_pi_reset(&pi);
    for (int i = 0; i < 20; i++)
        d = flowcalc_pi_step(&pi, &c, 5.0, 0.5);       /* far over the step */
    CHECK(d == 2.0, "far over the step: the duty held at its least");
    flowcalc_cfg_t wild = { 9.0, -3.0, 80.0 };
    flowcalc_cfg_t held = flowcalc_cfg_clamp(wild);
    CHECK(held.hold_c == FLOWCALC_HOLD_C_HI && held.duty_min == FLOWCALC_DUTY_MIN_LO &&
          held.duty_max == FLOWCALC_DUTY_MAX_HI, "each setting held to its range");
    flowcalc_cfg_t bad = { NAN, 1.0, NAN };
    held = flowcalc_cfg_clamp(bad);
    CHECK(held.hold_c == FLOWCALC_HOLD_C_DEF && held.duty_min == 1.0 && held.duty_max == FLOWCALC_DUTY_MAX_DEF,
          "a value that is not a number takes its default");
    CHECK(FLOWCALC_DUTY_MIN_HI < FLOWCALC_DUTY_MAX_LO, "the ranges keep the least under the most");
    flowcalc_ring_t ring;
    flowcalc_ring_reset(&ring);
    for (int i = 0; i < 179; i++)
        flowcalc_ring_push(&ring, 4.0);
    CHECK(isnan(flowcalc_fall(&ring)), "no fall before 90 s are held");
    flowcalc_ring_push(&ring, 4.0);
    CHECK(fabs(flowcalc_fall(&ring)) < 1e-12, "a steady duty: no fall");
    for (int i = 0; i < 20; i++)
        flowcalc_ring_push(&ring, 3.2);
    CHECK(fabs(flowcalc_fall(&ring) - 0.20) < 1e-9 && fabs(flowcalc_ring_mean(&ring, 10.0, 0.0) - 3.2) < 1e-9,
          "10 s at 80 %: a 20 % fall, the 10 s mean 3.2");
}

/* --- the model --------------------------------------------------------- */

typedef struct {
    double t, D, U, coolant, heater, g_scale;
    int dead, run_fans, no_means, stuck;
    double ring_d[50], ring_u[50];
    int nring, ring_i;
    double next_read;
    int session, fans, enabled, yield;
    const char *yield_why;
    flowcalc_cfg_t cfg;
} plant_t;

static fw_t W;
static plant_t P;

static void plant_reset(int enabled)
{
    memset(&P, 0, sizeof(P));
    P.t = 1000.0;
    P.coolant = 22.0;
    P.enabled = enabled;
    P.fans = FW_FANS_IDLE;
    P.cfg = flowcalc_cfg_default();
    fw_init(&W, cap_log);
    logbuf[0] = '\0';
}

#define DT 0.01

static void run(double s)
{
    double until = P.t + s;
    while (P.t < until) {
        P.t += DT;
        int flowing = !P.dead;
        double tau = flowing ? 7.0 : 11.0;
        double g = (flowing ? (P.run_fans ? 0.30 / 1.28 : 0.30) : 0.60) * (P.g_scale > 0.0 ? P.g_scale : 1.0);
        double share = flowing ? 0.1 : 0.3;
        if (!P.stuck) {
            P.D += DT * (g * P.heater - P.D) / tau;
            P.U += DT * (g * share * P.heater - P.U) / tau;
        }
        if (P.t >= P.next_read) {
            P.next_read += 0.02;
            P.ring_d[P.ring_i] = P.coolant + P.D;
            P.ring_u[P.ring_i] = P.coolant + 0.05 + P.U;
            P.ring_i = (P.ring_i + 1) % 50;
            if (P.nring < 50)
                P.nring++;
        }
        fw_in_t in;
        memset(&in, 0, sizeof(in));
        in.now = P.t;
        if (P.nring == 50 && !P.no_means) {
            double sd = 0, su = 0;
            for (int i = 0; i < 50; i++) {
                sd += P.ring_d[i];
                su += P.ring_u[i];
            }
            in.have_means = 1;
            in.down_c = sd / 50;
            in.up_c = su / 50;
        }
        in.enabled = P.enabled;
        in.cfg = P.cfg;
        in.yield = P.yield;
        in.yield_why = P.yield_why;
        in.session = P.session;
        in.fans = P.fans;
        P.heater = fw_step(&W, &in);
    }
}

/* The watch stepped with given readings, no model behind them: @s seconds
 * from *t at these 1 s means, session and fans. Returns the heater asked. */
static double feed(double *t, double s, double down_c, double up_c, int session, int fans)
{
    double until = *t + s, heater = 0.0;
    while (*t < until) {
        *t += 0.05;
        fw_in_t in;
        memset(&in, 0, sizeof(in));
        in.now = *t;
        in.have_means = 1;
        in.down_c = down_c;
        in.up_c = up_c;
        in.enabled = 1;
        in.cfg = P.cfg;
        in.session = session;
        in.fans = fans;
        heater = fw_step(&W, &in);
    }
    return heater;
}

static void test_start(void)
{
    printf("B. a start: the rest, the hold, the idle reference, the summary\n");
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(9.0);
    CHECK(W.mode == FW_REST && P.heater == 0.0, "the rest: the heater off");
    run(2.0);
    CHECK(W.mode == FW_HOLD && log_has("the hold starts"), "then the hold");
    run(95.0);
    char doc0[512];
    fw_status_json(&W, doc0, sizeof(doc0));
    CHECK(strstr(doc0, "\"fall\":null") != NULL, "/cool/flow gives no fall while the detector is blind");
    CHECK(W.have_ref && fabs(W.ref - 1.0 / 0.27) < 0.2 && log_has("the idle reference is"),
          "the idle reference from the settled duty, logged");
    run(600.0);
    CHECK(log_has("flow: idle: ") && !log_has("flow: fall:"), "an idle summary, and no fall while flowing");
    double ref1 = W.ref;
    P.cfg.hold_c = 2.0;                 /* the owner doubles the step */
    run(1.0);
    CHECK(log_has("settings changed (step 2.00 C") && !W.have_ref && W.mode == FW_HOLD,
          "a settings change starts the hold over, its idle reference with it");
    run(100.0);
    CHECK(W.have_ref && fabs(W.ref / ref1 - 2.0) < 0.15, "the new reference at twice the step costs about twice");
    char doc[256];
    fw_status_json(&W, doc, sizeof(doc));
    CHECK(strstr(doc, "\"hold_c\":2.00") && strstr(doc, "\"duty_max\":15.0"), "/cool/flow carries the settings in force");
}

static void test_idle_stop(void)
{
    printf("C. a pump that quits at idle\n");
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(200.0);
    P.dead = 1;
    double t0 = P.t;
    while (!log_has("flow: fall:") && P.t - t0 < 60.0)
        run(0.5);
    CHECK(log_has("flow: fall:") && log_has("at idle"), "the fall detector names it");
    CHECK(P.t - t0 < 30.0, "within 30 s");
}

static void test_job(void)
{
    printf("D. a job: a flowing one, then a stop under one\n");
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(120.0);
    /* The idle duty a little under its reference, as its own noise puts it
     * half the time; then the run fans asked for, not yet up. */
    P.g_scale = 1.05;
    run(60.0);
    P.session = 1;
    P.fans = FW_FANS_RUN;
    run(8.0);
    P.g_scale = 1.0;
    P.run_fans = 1;
    run(292.0);
    P.session = 0;
    P.fans = FW_FANS_IDLE;
    P.run_fans = 0;
    run(1.0);
    CHECK(log_has("flow: job:") && log_has("the floor never crossed") && log_has("the fall detector never tripped"),
          "a flowing job crosses neither, and says so");
    logbuf[0] = '\0';
    run(200.0);
    P.session = 1;
    P.fans = FW_FANS_RUN;
    P.run_fans = 1;
    run(200.0);
    P.dead = 1;
    double stop = P.t;
    run(60.0);
    CHECK(log_has("flow: floor:") && W.floor_noted && W.session_at + W.floor_at - stop < 30.0,
          "the floor names the stop within 30 s");
    CHECK(log_has("flow: fall:") && log_has("under a job"), "the fall detector names it");
    P.session = 0;
    P.fans = FW_FANS_IDLE;
    run(1.0);
    CHECK(log_has("the floor would have held it at") && log_has("the fall detector would have held it at"),
          "the job's summary carries both");

    /* A job before the first reference: the reference comes from the idle
     * after it, its own 60 to 90 s at the idle fans. */
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(40.0);
    P.session = 1;
    P.fans = FW_FANS_RUN;
    P.run_fans = 1;
    run(120.0);
    P.session = 0;
    P.fans = FW_FANS_IDLE;
    P.run_fans = 0;
    run(85.0);
    CHECK(!W.have_ref, "a job before the reference: none yet, 85 s into the idle after it");
    run(10.0);
    CHECK(W.have_ref && fabs(W.ref - 1.0 / 0.27) < 0.2, "then the reference, from that idle");
}

static void test_yield(void)
{
    printf("E. the engine takes the heater back\n");
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(60.0);
    CHECK(P.heater > 0.0, "the hold heats");
    P.yield = 1;
    P.yield_why = "warm-up";
    run(0.02);
    CHECK(P.heater == 0.0 && W.mode == FW_SUSPENDED, "off at once");
    char doc[512];
    fw_status_json(&W, doc, sizeof(doc));
    CHECK(strstr(doc, "\"watch\":\"suspended\"") && strstr(doc, "\"why\":\"warm-up\""), "/cool/flow says why it paused");
    run(30.0);
    CHECK(P.heater == 0.0, "and nothing while the engine has it");
    P.yield = 0;
    run(30.0);
    CHECK(W.mode == FW_REST && P.heater == 0.0, "back: a full rest first");
    run(35.0);
    CHECK(W.mode == FW_HOLD, "then the hold");
}

static void test_setting(void)
{
    printf("F. cool_flow_watch 0, the default\n");
    plant_reset(0);
    fw_begin(&W, P.t, 1e9);
    run(120.0);
    CHECK(W.mode == FW_OFF && P.heater == 0.0 && log_has("the watch is off"), "off: nothing heated, said once");
    P.session = 1;
    P.fans = FW_FANS_RUN;
    run(20.0);
    P.session = 0;
    P.fans = FW_FANS_IDLE;
    run(1.0);
    CHECK(!log_has("flow: job:"), "off: a job leaves no flow line");
    P.enabled = 1;
    run(1.0);
    P.session = 1;
    P.fans = FW_FANS_RUN;
    run(5.0);
    P.session = 0;
    P.fans = FW_FANS_IDLE;
    run(24.0);
    CHECK(log_has("no hold at the run fans under it (the watch: rest)"), "on, still resting: the job's one line says so");
    CHECK(W.mode == FW_REST && P.heater == 0.0, "turned on: a rest first");
    run(35.0);
    CHECK(W.mode == FW_HOLD && P.heater > 0.0, "then the hold");
    P.enabled = 0;
    run(0.02);
    CHECK(W.mode == FW_OFF && P.heater == 0.0, "turned off: the heater off at once");
}

static void test_faults(void)
{
    printf("G. no readings; a pinned hold; the rail\n");
    plant_reset(1);
    fw_begin(&W, P.t, 1e9);
    run(30.0);
    CHECK(P.heater > 0.0, "the hold heats");
    P.no_means = 1;
    run(2.5);
    CHECK(P.heater == 0.0 && log_has("no readings"), "no means: the heater off within 2 s, named");
    P.no_means = 0;
    run(5.0);
    CHECK(P.heater > 0.0, "the readings back: the hold again");
    P.stuck = 1;
    P.D = P.U = 0.0;
    run(110.0);
    CHECK(W.mode == FW_OFF && W.halted && P.heater == 0.0, "pinned at its most: stopped, heater off");
    char doc[512];
    fw_status_json(&W, doc, sizeof(doc));
    CHECK(strstr(doc, "\"halted\":true") && strstr(doc, "dead reading"), "/cool/flow says it stopped, and why");
    P.enabled = 0;
    run(6.0);
    P.enabled = 1;
    run(70.0);
    CHECK(W.mode == FW_OFF && P.heater == 0.0, "the setting does not wake it before the next start");

    /* The rail, by its number: both sensors together, so the hold always
     * wants heat, a half degree either side of 45 C. */
    plant_reset(1);
    double t = P.t;
    fw_begin(&W, t, 1e9);
    feed(&t, 15.0, 44.5, 44.5, 0, FW_FANS_IDLE);
    CHECK(W.mode == FW_HOLD && feed(&t, 5.0, 44.5, 44.5, 0, FW_FANS_IDLE) > 0.0, "downstream at 44.5 C: the hold heats");
    CHECK(feed(&t, 1.0, 45.5, 45.5, 0, FW_FANS_IDLE) == 0.0, "at 45.5 C: the heater off");
    CHECK(feed(&t, 1.0, 44.5, 44.5, 0, FW_FANS_IDLE) > 0.0, "at 44.5 C again: the hold heats");
}

static void test_no_reference(void)
{
    printf("H. a hold that settles at no duty\n");
    plant_reset(1);
    double t = P.t;
    fw_begin(&W, t, 1e9);
    feed(&t, 11.0, 22.0, 22.0, 0, FW_FANS_IDLE);            /* the rest: no difference */
    /* Downstream 2 C over upstream with no heat: over the step, the hold
     * sits at its least, which is no duty at all. */
    CHECK(feed(&t, 95.0, 24.0, 22.0, 0, FW_FANS_IDLE) == 0.0 && W.mode == FW_HOLD, "the hold costs nothing");
    CHECK(!W.have_ref && isnan(W.ref), "no idle reference is taken from it");
    /* A job after it whose hold does cost duty: nothing to judge it against. */
    CHECK(feed(&t, 40.0, 22.0, 22.0, 1, FW_FANS_RUN) > 0.0, "under the job the hold heats");
    CHECK(!W.floor_noted && isnan(W.low_ratio), "the floor judged nothing under the job");
    /* The job's end writes its summary; section I counts every line, and
     * none holds an inf or a nan. Then an idle stretch held on the step, at
     * a steady duty. */
    feed(&t, 95.0, 23.0, 22.0, 0, FW_FANS_IDLE);
    CHECK(W.have_ref && W.ref > 0.0, "the idle stretch after it, at a duty, gives the reference");
}

int main(void)
{
    test_calc();
    test_start();
    test_idle_stop();
    test_job();
    test_yield();
    test_setting();
    test_faults();
    test_no_reference();
    printf("I. every line logged above\n");
    CHECK(nan_lines == 0, "no line prints a number that was never measured (nan, inf)");
    printf(failures ? "flowwatch_test: %d FAILED\n" : "flowwatch_test: all ok\n", failures);
    return failures ? 1 : 0;
}
