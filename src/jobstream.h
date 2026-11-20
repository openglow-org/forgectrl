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
 * the emission witnesses sampled at 25 Hz (or a fraction of it, for a
 * long job that wants the record and not the trace): the tube current,
 * the head thermopile, the kernel's LASER_ON sample count, and the lid IR
 * quartet. The job runner is its one caller (jobrun.h).
 *
 * The run is synchronous on the caller's thread and ends when every
 * line is acknowledged and, if asked, the tube has been dark for a
 * while after; a NULL line from the generator ends the program. A run
 * that fails or is aborted sends the controller a soft reset (^X: a
 * controlled stop, the latch relocked) before it closes; on a connection
 * of its own when the controller closed the run's. A reset the
 * controller reports (its banner a second time on the run's connection:
 * a lid cancel, a sender's ^X) ends the run at once, failed, with no
 * reset of the run's own: the controller has stopped the run itself.
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
    int sample_div;                     /* the witnesses every this many ticks (0 or 1: every tick) */
} jobstream_cfg_t;

/* Live counters the caller may read from another thread for progress. */
typedef struct {
    volatile int sent, acked;
    volatile int samples;
    volatile int lit;                   /* a discharge has been seen */
    double t_first_lit, t_last_lit;
    long hv_max, lon_max;
    long tp_base, tp_max;               /* thermopile: the dark level, the peak */
    int own_stop;                       /* the run ended with a soft reset of its own: not after
                                         * the controller's alarm, nor its alarm lock (error:9),
                                         * nor its own reset */
} jobstream_run_t;

/* The GRBL controller's published state (<run>/grbl.state), whole and
 * current: 0 with the document in body, or -1 when it is missing, torn
 * (no closing brace, or more than the buffer holds), carries no ts_mono,
 * or is older than JOBSTREAM_STATE_STALE_S on CLOCK_MONOTONIC. The
 * controller rewrites it on every change and on a 5 s heartbeat, so a
 * live controller's file is never that old: one that is has stopped being
 * written, and what it last said is not known. A stamp up to a second
 * ahead of the reader is the same moment. Every reader of the file that
 * decides something reads it here. */
#define JOBSTREAM_STATE_STALE_S 10.0
#define JOBSTREAM_STATE_MAX     1024    /* the controller's document is under 700 bytes */
int jobstream_grbl_state(char *body, size_t len);

/* Is a sender on the Grbl socket (from the controller's published
 * state)? 1, 0, or -1 when the state is unreadable or not current. */
int jobstream_sender_connected(void);
/* Whether a run of the daemon's own must be refused for a sender: a
 * connected one, and a state that cannot be read or is not current - a
 * state nobody can vouch for is no license to displace whoever holds the
 * socket. */
int jobstream_sender_blocks(void);

/* Run a program. 0 when every line was acknowledged and the end
 * condition held; -1 with the reason in err (the controller's error or
 * ALARM with the line it answered, the connection lost, a timeout, an
 * abort, the generator's failure). */
int jobstream_run(const jobstream_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen);

/* A generator over an array of lines (a program in memory). A line that
 * does not fit the buffer fails the run: it is never split. */
typedef struct {
    const char *text;       /* newline-separated lines */
    size_t off;
} jobstream_lines_t;
int jobstream_lines_gen(void *ctx, char *buf, size_t len);

#endif
