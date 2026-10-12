/*
 * lensfind.c - the lens's free travel, found by the head accelerometer
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The Lens travel check's measurement (wizdark.c), run with the
 * controller stopped and the lens on its hall edge: the lens pulsed one
 * half-step at a time over sysfs, every fan off, the accelerometer
 * listened to after every step.
 */
#define _GNU_SOURCE
#include "lensfind.h"
#include "accel.h"
#include "cool.h"
#include "wizcalc.h"
#include "wizrun.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define Z_STEP_MS     180       /* the direct-step cadence of every lens reference and the finder */

/* The head's stops, found by the head accelerometer one half-step at a
 * time from the hall edge: a free step rings on every second half-step,
 * and at a stop the ring dies two to four steps before the rotor slips
 * (the bench reference machine, 2026-09-06). Contact is called on the
 * first quiet strong-parity step, the lens backs off two, and the count
 * back to the edge must match the steps taken. A slip, an unreadable
 * ring, or no contact within reach means the stops are not findable on
 * this head: the fallback window stands and the user is told. */
#define FIND_MAX       30
#define FIND_MIN       8        /* a stop nearer the edge than this is a misread: the factory's own
                                   zero sits 8 half-steps below the edge on every head */
#define FIND_FLOOR     6000L    /* the strong level a readable ring has at least, summed */
#define FIND_BACK      2
#define FIND_QUIET_S   10       /* the fans off this long before the listening */
#define FIND_WINDOW_MS 170
#define FIND_TOL       1

static int z_hall_home(void)
{
    long h = wiz_rd_long("head/hall_sensor", -1);
    return h < 0 ? -1 : h == 0;
}

/* Step `up` until the hall reads `home`; the steps taken, or -1. */
static int z_until(int home, int up, int max)
{
    for (int c = 0; c < max; c++) {
        int h = z_hall_home();
        if (h < 0)
            return -1;
        if (h == home)
            return c;
        if (wiz_wr_attr("cnc/z_step", up ? "1" : "0") != 0)
            return -1;
        wiz_msleep(Z_STEP_MS);
        if (wiz_aborted())
            return -1;
    }
    return -1;
}

/* One half-step with the accelerometer listened to through the cadence
 * window: the three axes' peak-to-peak, summed. */
static int z_step_listen(int up, long *sum)
{
    long lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }, v[3];
    int n = 0;
    struct timespec t0, t;
    if (wiz_wr_attr("cnc/z_step", up ? "1" : "0") != 0)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (crash_hw_burst(&v[0], &v[1], &v[2]) != 0)
            return -1;
        for (int i = 0; i < 3; i++) {
            if (n == 0 || v[i] < lo[i])
                lo[i] = v[i];
            if (n == 0 || v[i] > hi[i])
                hi[i] = v[i];
        }
        n++;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= FIND_WINDOW_MS)
            break;
    }
    wiz_msleep(Z_STEP_MS - FIND_WINDOW_MS);
    if (wiz_aborted())
        return -1;
    *sum = (hi[0] - lo[0]) + (hi[1] - lo[1]) + (hi[2] - lo[2]);
    return 0;
}

/* One leg from the edge toward a stop; the lens is backed off after
 * whatever was seen. 0 with *contact, else -1 with `why`. */
static int z_find_leg(int up, int *contact, char *why, size_t wlen)
{
    long sums[FIND_MAX];
    int n = 0, slipped = 0, call = 0, rc = 0;
    const char *which = up ? "top" : "bottom";
    while (n < FIND_MAX && !call) {
        wiz_phase("finding the %s stop by the head accelerometer: %d half-steps %s the edge",
                  which, n + 1, up ? "above" : "below");
        if (z_step_listen(up, &sums[n]) != 0) {
            snprintf(why, wlen, "the head accelerometer could not be read");
            rc = -1;
            break;
        }
        n++;
        call = wizcalc_stop_call(sums, n, &slipped);
    }
    for (int i = 0; i < FIND_BACK; i++) {
        wiz_wr_attr("cnc/z_step", up ? "0" : "1");
        wiz_msleep(Z_STEP_MS);
    }
    /* The ring, step by step in hundreds, for the record and for a head
     * whose stops could not be found; the log line is short, so two of
     * them. */
    for (int from = 0; from < n; from += 15) {
        char ring[15 * 5 + 1];
        int len = 0;
        for (int i = from; i < n && i < from + 15; i++)
            len += snprintf(ring + len, sizeof(ring) - len, "%s%ld", i > from ? " " : "",
                            (sums[i] + 50) / 100);
        wiz_log("%s ring %d-%d (x100): %s", which, from + 1, n < from + 15 ? n : from + 15, ring);
    }
    if (rc != 0)
        return -1;
    if (!call) {
        snprintf(why, wlen, "no %s stop within %d half-steps of the edge", which, FIND_MAX);
        return -1;
    }
    if (slipped) {
        snprintf(why, wlen, "the lens slipped at the %s stop before its ring died", which);
        return -1;
    }
    if (!wizcalc_ring_readable(sums, call, FIND_FLOOR)) {
        snprintf(why, wlen, "the lens steps do not ring clearly enough on this head");
        return -1;
    }
    if (call - FIND_BACK < FIND_MIN) {
        snprintf(why, wlen, "the %s stop was called %d half-steps from the edge, nearer than any head's",
                 which, call - FIND_BACK);
        return -1;
    }
    wiz_log("the %s stop: the ring died %d half-steps %s the edge", which, call, up ? "above" : "below");
    *contact = call;
    return 0;
}

int lensfind_run(lensfind_t *st, json_t *r)
{
    memset(st, 0, sizeof(*st));
    int opened = 0;
    wiz_wr_attr("head/z_current", "0");
    wiz_wr_attr("head/z_mode", "1");
    wiz_wr_attr("head/z_enable", "0");
    /* Every fan off, and a set time for them to run down, before the
     * accelerometer is listened to: a running fan is in the reading. */
    cool_quiet_hold(1);
    for (int i = 0; i < FIND_QUIET_S; i++) {
        wiz_phase("the fans stop before the listening: %d s of %d", i, FIND_QUIET_S);
        wiz_msleep(1000);
        if (wiz_aborted()) {
            snprintf(st->why, sizeof(st->why), "aborted");
            goto done;
        }
    }
    /* The listener takes the crash watch's bus handle, or opens its
     * own for the finding and closes it after. */
    opened = crash_hw_listen(1) == 0;
    if (!opened) {
        snprintf(st->why, sizeof(st->why), "the head accelerometer does not answer on its bus");
        goto done;
    }
    int c;
    if (z_find_leg(0, &st->contact_below, st->why, sizeof(st->why)) != 0)
        goto done;
    /* The count back to the edge is the steps taken, or something slipped. */
    c = z_until(1, 1, 80);
    if (c < 0 || c < st->contact_below - FIND_BACK - FIND_TOL || c > st->contact_below - FIND_BACK + FIND_TOL) {
        snprintf(st->why, sizeof(st->why), "the count back from the bottom stop was %d for %d steps taken",
                 c, st->contact_below - FIND_BACK);
        goto done;
    }
    if (z_find_leg(1, &st->contact_above, st->why, sizeof(st->why)) != 0)
        goto done;
    /* Down until the hall lets go (the steps taken plus its band), then
     * up to the edge again. */
    c = z_until(0, 0, 80);
    if (c < 0 || c - (st->contact_above - FIND_BACK) < 2 || c - (st->contact_above - FIND_BACK) > 9) {
        snprintf(st->why, sizeof(st->why), "the count back from the top stop was %d for %d steps taken",
                 c, st->contact_above - FIND_BACK);
        goto done;
    }
    if (z_until(1, 1, 60) < 0) {
        snprintf(st->why, sizeof(st->why), "the hall edge was lost after the top stop");
        goto done;
    }
    st->below = st->contact_below - FIND_BACK;
    st->above = st->contact_above - FIND_BACK;
    st->found = 1;
done:
    if (opened)
        crash_hw_listen(0);
    cool_quiet_hold(0);
    if (!st->found) {
        wiz_log("the stops could not be found: %s", st->why);
        /* Whatever happened, the lens ends on the edge. */
        if (z_hall_home() == 1)
            z_until(0, 0, 60);
        z_until(1, 1, 80);
    }
    /* The lens motor back at its hold current, as every session ends. */
    wiz_wr_attr("head/z_current", "1");
    json_object_set_new(r, "stops",
                        json_pack("{s:b,s:i,s:i,s:i,s:i,s:s}", "found", st->found, "below", st->below,
                                  "above", st->above, "contact_below", st->contact_below,
                                  "contact_above", st->contact_above, "why", st->why));
    return 0;
}
