/*
 * wizcalc.h - the dark wizards' decisions, apart from the hardware
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Pure functions over samples: what the airflow wizard makes of a
 * tach trace, what the sensors wizard makes of a sample at rest, the
 * heater-duty ladder of the flow calibration, and the agreement of the
 * lens reference passes. The host tests run these against recorded
 * traces; wizdark.c feeds them the machine.
 */
#ifndef FORGECTRL_WIZCALC_H
#define FORGECTRL_WIZCALC_H

#include <stddef.h>

/* One fan's run-duty trace: `rpm[0..n)` sampled at `hz`. steady_rpm is
 * the median of the last 10 s; spinup_s the time the reading first
 * reached 90 percent of steady; ok when steady is at least min_rpm.
 * Returns -1 when the trace is too short to judge (under 12 s). */
typedef struct {
    double steady_rpm;
    double spinup_s;
    int ok;
} wizcalc_fan_t;
int wizcalc_fan_analyze(const double *rpm, int n, double hz, double min_rpm,
                        wizcalc_fan_t *out);

/* The floor a fan gets: `frac` of its steady speed, inside [lo, hi]. */
double wizcalc_floor(double steady, double frac, double lo, double hi);

/* The fan grace: the slowest spin-up plus 5 s, inside [lo, hi]. */
double wizcalc_grace_s(double spinup_s, double lo, double hi);

/* The flow calibration's heater duty ladder: 40, then 55, then 70
 * percent; -1 after the last rung. */
double wizcalc_next_duty(double duty);

/* A sample at rest, as the sensors wizard sees it. A reading it could
 * not take is NAN (temperatures) or -1 (counts); those are skipped. */
typedef struct {
    double down_c, up_c;        /* coolant, downstream and upstream */
    double chassis_c, soc_c;
    long ir[4];                 /* the lid IR quartet */
    double ir_alert;            /* the lower alert tier of the fire watch */
    int accel_events;           /* crash-watch events seen at rest */
    long pgood;                 /* laser supply power-good sampled, 0 or 1 */
    long hv_current;            /* counts at rest */
    double tach_exhaust, tach_intake;   /* rpm at the idle duty */
} wizcalc_sensors_t;

/* The number of readings that are not plausible at rest, and the
 * reasons, comma separated, in why. 0 means every reading is fine. */
int wizcalc_sensors_judge(const wizcalc_sensors_t *s, char *why, size_t len);

/* The lens reference: the passes' positions agree within tol steps. */
int wizcalc_z_concur(const long *pos, int n, long tol);

/* The lens stop finder's call on the ring. sums[] are the head
 * accelerometer's summed peak-to-peak per half-step so far, n of them,
 * from the hall edge toward a stop. A free step rings strongly on every
 * second half-step: the strong parity is the louder of the first two,
 * and the strong and quiet levels are the medians of each parity's
 * steps before the step judged (the strong level takes the lower middle
 * of an even count, the quiet level the upper: both toward the gap;
 * nothing is judged before the fifth step, two of each). Contact is
 * called at the first strong-parity step under a quarter of the way up
 * from the quiet level to the strong (a stalled carriage rings at the
 * quiet level, the fans' floor after a burn; a weak free step keeps
 * most of its ring), or at any step over 1.8 times the strong level
 * (the rotor slipped: the stop was passed). Returns the 1-based step of
 * the call, 0 for none yet; *slipped is set on a burst. */
int wizcalc_stop_call(const long *sums, int n, int *slipped);

/* The ring is readable when the steps before `at` (1-based, exclusive)
 * hold at least two of each parity, the strong level is at least
 * `floor`, and it is at least 1.5 times the quiet level. */
int wizcalc_ring_readable(const long *sums, int at, long floor);

/* The jog witness: a head-accelerometer reading on each axis over the
 * jog (wizdark.c takes the low-passed peak-to-peak, the commanded
 * acceleration's own signature), against the same reading at rest just
 * before. The head moved when its busier axis reads at least `ratio`
 * times the busier rest axis and at least `floor` counts. */
int wizcalc_jog_witnessed(double rms_x, double rms_y, double rest_x, double rest_y,
                          double ratio, double floor);

/* One browser drives a running wizard; a second one mirrors it. The
 * owner is the login session that started the run ("" when a tool with
 * the token and no session did). A requester may answer, abort, or
 * start when it is the owner, when the run has no owner, or when it has
 * no session itself (the bench tools and the init scripts, which
 * cannot be told apart and never mirror). */
int wizcalc_may_act(const char *owner, const char *requester);

#endif
