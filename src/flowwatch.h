/*
 * flowwatch.h - the coolant flow watch: the hold's duty, logged, enforcing nothing
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Opt-in (cool_flow_watch 1) and log-only: no fire gate, no hold, no refused
 * job, and the pump is never touched. It collects what the hold sees on
 * machines other than the bench reference, whose flow board may differ (an
 * inline sensor housing on early machines, one molded into the reservoir on
 * later ones).
 *
 * - The hold, from the start: after a rest with the heater off (the
 *   difference at rest measured over its last 10 s), a PI loop holds
 *   downstream a step over upstream (flowcalc.h; cool_flow_hold_c, 1 C by
 *   default, between cool_flow_duty_min and cool_flow_duty_max), and the duty
 *   it costs is the signal. Its settled duty at the idle fans, 60 to 90 s
 *   after it starts, is the idle reference; a hold that settles at no duty
 *   gives none, and says so (nothing can be measured against zero). A change
 *   to those settings starts the hold over, and the idle reference with it.
 * - The fall detector: the 10 s duty 15 % or more under its own mean 60 to 90
 *   s earlier, blind for 150 s after the hold starts or the fans change. A
 *   pump that quits while running shows this way. A pump that has not moved
 *   since the start does not: the duty means something only as a change.
 * - Under a job at the run fans, the floor: the 10 s duty under the idle
 *   reference (the only signal in the fall detector's blind start).
 * - Logged: the idle reference; at idle every FW_SUMMARY_S the duty's mean,
 *   its 10 s range and the largest fall seen; the first time the fall
 *   detector or the floor would have held a job; and a summary at each job's
 *   end. GET /cool/flow carries the state.
 *
 * It gives the heater back whenever the cooling engine needs it (a warm-up,
 * diagnostics, a quiet hold, an unreadable sensor) and starts over with a
 * fresh rest after.
 *
 * Pure: no clock, no I/O. flowrun.c reads the sampler's means, steps it and
 * writes the heater it asks for.
 */
#ifndef FORGECTRL_FLOWWATCH_H
#define FORGECTRL_FLOWWATCH_H

#include "flowcalc.h"

#include <stddef.h>

#define FW_REST_MIN_S      10.0    /* the rest before the hold, at least */
#define FW_REST_AFTER_HEAT 60.0    /* the heater off this long before a rest is a rest */
#define FW_REST_MEAN_S     10.0    /* the difference at rest: this many seconds' mean */
#define FW_REF_FROM_S      60.0    /* the idle reference: the hold's duty from here */
#define FW_REF_TO_S        90.0    /* ... to here after it starts */
#define FW_BLIND_S        150.0    /* the fall detector's blind time after a start or a fan change */
#define FW_FLOOR_SETTLE_S  30.0    /* the floor judges from this long at the run fans: before it, the
                                    * 10 s duty is still the idle fans' and sits on the reference */
#define FW_RAIL_C          45.0    /* downstream at or over this: the heater off */
#define FW_HIGH_S          20.0    /* the hold at its most this long: a frozen or dead reading */
#define FW_SUMMARY_S      600.0    /* an idle summary this often */

typedef enum { FW_FANS_IDLE = 0, FW_FANS_RUN, FW_FANS_COOL, FW_FANS_OTHER } fw_fans_t;

typedef enum {
    FW_WAIT = 0,      /* not begun: touches nothing */
    FW_REST,          /* the rest before the hold */
    FW_HOLD,
    FW_SUSPENDED,     /* the engine has the heater */
    FW_OFF,           /* turned off, or stopped for the start */
} fw_mode_t;

typedef struct {
    double now;                 /* seconds, CLOCK_MONOTONIC */
    int have_means;             /* the sampler's 1 s means */
    double down_c, up_c;
    int enabled;                /* cool_flow_watch */
    flowcalc_cfg_t cfg;         /* the hold's step and duty limits, held to their ranges */
    /* From the cooling engine's last tick. */
    int yield;                  /* the engine needs the heater */
    const char *yield_why;
    int session;                /* a run session at the run fans */
    int fans;                   /* fw_fans_t */
} fw_in_t;

typedef void (*fw_log_fn)(int prio, const char *fmt, ...);

typedef struct {
    fw_log_fn log;
    fw_mode_t mode;
    int halted;                 /* stopped until the next start (no heater or a dead reading) */
    const char *yield_why;      /* why the engine has the heater (FW_SUSPENDED); a string literal */
    double fall_now;            /* the fall detector's last judged fall; NAN while it is blind */
    double heater;              /* the duty asked, percent */
    double heater_off_at;
    double last_means_at;
    int means_lost_noted;
    /* The rest and the difference at rest. */
    double rest_until, rest_sum, next_rest_sample, rest_c;
    int rest_n, have_rest;
    /* The hold. */
    flowcalc_cfg_t cfg;         /* in force; a change starts the hold over */
    flowcalc_pi_t pi;
    flowcalc_ring_t ring;
    double duty, next_pi, hold_start;
    int high_steps;
    double ref;
    int have_ref, seg_ref_noted;
    double fans_changed_at;
    int last_fans;
    int fall_noted;
    /* The idle summary. */
    double sum_at, sum_duty, sum_lo, sum_hi, sum_fall;
    int sum_n;
    /* The job. */
    int session;
    double session_at;
    int floor_noted;
    double floor_at, fall_at, worst_fall, low_ratio, run_sum;
    int run_n;
} fw_t;

void fw_init(fw_t *w, fw_log_fn log);
/* Begin at a start. heater_off_ago: seconds since this daemon's heater last
 * ran (huge when unknown or never). */
void fw_begin(fw_t *w, double now, double heater_off_ago);
/* One step; returns the heater duty wanted, percent. */
double fw_step(fw_t *w, const fw_in_t *in);
/* A one-line JSON summary for /cool/flow. */
int fw_status_json(const fw_t *w, char *buf, size_t len);
const char *fw_mode_name(fw_mode_t m);

#endif
