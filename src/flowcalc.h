/*
 * flowcalc.h - the coolant flow watch's arithmetic: the hold and its falls
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The hold: a PI loop on the flow board's heater duty holds the downstream
 * sensor a fixed step over the upstream one, counted from their difference
 * at rest. Moving coolant carries the heat away, so holding the step costs
 * more duty; still coolant lets the board keep its heat, so it costs less.
 * The duty is the signal, and only as a change on the same machine: what a
 * flowing loop costs differs from machine to machine.
 *
 * Pure: no clock, no I/O.
 */
#ifndef FORGECTRL_FLOWCALC_H
#define FORGECTRL_FLOWCALC_H

#include <stddef.h>

#define FLOWCALC_KP        2.0      /* percent per C */
#define FLOWCALC_KI        0.2      /* percent per C second */

/* The owner's three numbers (cool_flow_hold_c, cool_flow_duty_min,
 * cool_flow_duty_max), each held to its range. The step: under 0.5 C the
 * error in the difference at rest (about 0.1 C on the bench reference)
 * swamps it; over 3 C the hold costs about three times the heat. The duty:
 * the least never reaches the most's lowest value, so min < max by range
 * alone; the most stays over the flowing duty at the run fans (about 4.7 %
 * on the bench reference) so a moving loop never pins it, and under what
 * would drive a stopped board hard. */
#define FLOWCALC_HOLD_C_DEF      1.0
#define FLOWCALC_HOLD_C_LO       0.5
#define FLOWCALC_HOLD_C_HI       3.0
#define FLOWCALC_DUTY_MIN_DEF    0.0
#define FLOWCALC_DUTY_MIN_LO     0.0
#define FLOWCALC_DUTY_MIN_HI     4.0
#define FLOWCALC_DUTY_MAX_DEF   15.0
#define FLOWCALC_DUTY_MAX_LO     5.0
#define FLOWCALC_DUTY_MAX_HI    25.0
typedef struct {
    double hold_c;                  /* the step held over the difference at rest, C */
    double duty_min, duty_max;      /* the hold's duty limits, percent */
} flowcalc_cfg_t;
/* The defaults. */
flowcalc_cfg_t flowcalc_cfg_default(void);
/* Each value held to its range; a value that is not a number takes its default. */
flowcalc_cfg_t flowcalc_cfg_clamp(flowcalc_cfg_t c);

typedef struct {
    double integ;                   /* the integral term, percent */
} flowcalc_pi_t;
void flowcalc_pi_reset(flowcalc_pi_t *pi);
/* pv: the difference over its rest value, C. Returns the duty, percent,
 * clamped to the config's limits, with the integral held at the rails. */
double flowcalc_pi_step(flowcalc_pi_t *pi, const flowcalc_cfg_t *c, double pv, double dt);

/* The hold's duty, one value every FLOWCALC_DUTY_DT seconds, the oldest
 * dropped. */
#define FLOWCALC_DUTY_DT    0.5
#define FLOWCALC_DUTY_N     240     /* 120 s */
typedef struct {
    double v[FLOWCALC_DUTY_N];
    size_t head;                    /* the next slot */
    size_t count;                   /* values held, at most FLOWCALC_DUTY_N */
} flowcalc_ring_t;
void flowcalc_ring_reset(flowcalc_ring_t *r);
void flowcalc_ring_push(flowcalc_ring_t *r, double v);
/* The mean of the values from `from_s` seconds ago up to `to_s` seconds ago
 * (from_s > to_s >= 0), NAN unless every one of them is held. */
double flowcalc_ring_mean(const flowcalc_ring_t *r, double from_s, double to_s);

/* The fall detector: the last 10 s mean against the mean from 90 to 60 s
 * before, as a share fallen (0.15 = 15 % lower). NAN until 90 s are held. */
#define FLOWCALC_FALL_LINE  0.15
double flowcalc_fall(const flowcalc_ring_t *r);

#endif
