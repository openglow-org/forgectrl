/*
 * jobstream.h - the daemon's own Grbl sender with a witness
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * One local connection to the controller, lines in flight up to half
 * the controller's RX ring with ok-per-line accounting (the ring can
 * never overrun, the planner is never starved on short segments, and
 * the arm's button wait simply holds the first laser-on line's ok for
 * as long as the operator takes; a $ command, M102, and the program end
 * go alone as barriers), the lines produced on demand by a generator, an
 * abort on the controller's error or ALARM, and, while the job plays,
 * the emission witnesses sampled at 25 Hz: the tube current, the head
 * thermopile, the kernel's LASER_ON sample count, and the lid IR quartet.
 * The dose-curve recorder and the sheet wizards are its clients.
 *
 * The run is synchronous on the caller's thread and ends when every
 * line is acknowledged and, if asked, the tube has been dark for a
 * while after; a NULL line from the generator ends the program. A run
 * that fails or is aborted sends the controller a soft reset (^X: a
 * controlled stop, the latch relocked) before it closes.
 */
#ifndef FORGECTRL_JOBSTREAM_H
#define FORGECTRL_JOBSTREAM_H

#include <stddef.h>

#define JOBSTREAM_HZ 25.0
#define JOBSTREAM_HV_ON 30      /* pic/hv_current above this: discharge present */

typedef struct {
    double t;               /* seconds since the run began */
    long hv;                /* pic/hv_current */
    long tp;                /* head/beam_detect_analog */
    long lon;               /* cnc/laser_on_sampled */
    long ir[4];             /* pic/lid_ir_1..4 */
} jobstream_sample_t;

/* The next line, without its newline, into buf: 1 when a line was
 * produced, 2 for none yet (asked again at the next tick, so a
 * generator can pace a dwell against the samples without blocking
 * them), 0 at the end of the program, -1 to fail the run with the
 * reason in buf. Called on the run's thread once the previous line is
 * acknowledged; the run's timeout keeps counting. */
typedef int (*jobstream_gen_fn)(void *ctx, char *buf, size_t len);
/* Every sample, on the run's thread. */
typedef void (*jobstream_sample_fn)(void *ctx, const jobstream_sample_t *s);

typedef struct {
    jobstream_gen_fn gen;
    jobstream_sample_fn sample;         /* may be NULL */
    void *ctx;
    const volatile int *abort_flag;     /* may be NULL */
    double wait_timeout_s;              /* budget until the first discharge (0: none expected) */
    double run_timeout_s;               /* the whole run */
    double end_dark_s;                  /* keep sampling this long after the last ack, dark */
} jobstream_cfg_t;

/* Live counters the caller may read from another thread for progress. */
typedef struct {
    volatile int sent, acked;
    volatile int samples;
    volatile int lit;                   /* a discharge has been seen */
    double t_first_lit, t_last_lit;
    long hv_max, lon_max;
    long tp_base, tp_max;               /* thermopile: the dark level, the peak */
} jobstream_run_t;

/* Is a sender on the Grbl socket (from the controller's published
 * state)? 1, 0, or -1 when the state is unreadable. */
int jobstream_sender_connected(void);

/* Run a program. 0 when every line was acknowledged and the end
 * condition held; -1 with the reason in err (the controller's error or
 * ALARM with the line it answered, the connection lost, a timeout, an
 * abort, the generator's failure). */
int jobstream_run(const jobstream_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen);

/* A generator over an array of lines (a program in memory). */
typedef struct {
    const char *text;       /* newline-separated lines */
    size_t off;
} jobstream_lines_t;
int jobstream_lines_gen(void *ctx, char *buf, size_t len);

#endif
