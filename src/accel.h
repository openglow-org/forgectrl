/*
 * accel.h - the head-accelerometer crash watch: two tiers on the
 * LIS2HH12's own interrupt generators
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The factory's head crash detector is the head accelerometer itself:
 * the LIS2HH12 carries two on-chip interrupt generators, and the
 * factory arms them per job from the pulse header's HA* tags. IG1
 * takes a per-axis threshold (the alert tier, a pause); IG2 takes one
 * shared threshold (the abort tier, a fail). Both latch into a
 * per-axis source register the watcher polls and clears by reading.
 * ForgeFIRM adopts the mechanism with local thresholds seeded from a
 * captured cut header: X alert 132, Y alert 112, abort 133, IG
 * register units at the +/-4 g run full scale (LSB = full scale/256,
 * ~15.6 mg). Z is never armed - gravity rides it - and the factory
 * ships Z zero in every captured header too.
 *
 * The generators only sample at a running ODR, and st_accel leaves
 * the part in power-down between one-shot reads, so the armed watch
 * owns CTRL1 (800 Hz) and CTRL4 (+/-4 g) for the window and restores
 * both on disarm; every poll re-asserts them, so a one-shot read
 * elsewhere costs at most a tick. The IG registers themselves are
 * ones st_accel never touches, so the watch runs over i2c-dev with
 * the driver bound and the liveness path untouched.
 *
 * Split like airflow.c: the tier logic is pure (the engine feeds the
 * latched sources, the host test feeds numbers); the i2c access sits
 * behind the crash_hw_* calls.
 *
 * The bus handle has one owner, the cooling engine's crash watch: it
 * claims the handle at its first armed window and keeps it. The setup
 * checks (Sensors, Motion, the Lens travel finder) borrow the part, and
 * a borrow is refused while the watch is armed, so a check never reads
 * a source register the watch is waiting on (a read clears the latch)
 * and never turns its generators off. An armed window that opens while
 * a check holds the part takes it: the borrow ends there, and the
 * check's later calls are refused and touch nothing. A check's return
 * closes the handle only when its borrow opened it and the engine has
 * not claimed it since. One lock covers the handle and every register
 * transaction, so the engine's thread and a check's never interleave on
 * the bus.
 */
#ifndef FORGECTRL_ACCEL_H
#define FORGECTRL_ACCEL_H

/* IG_SRC1/IG_SRC2 bits: IA = any latched event, XH/YH the high-event
 * axis sources (the low-event and Z bits are never armed). */
#define CRASH_SRC_IA  0x40
#define CRASH_SRC_XH  0x02
#define CRASH_SRC_YH  0x08

/* A standing alert releases after this many consecutive quiet polls
 * (1 Hz ticks), the fire watch's release shape. */
#define CRASH_CLEAR_TICKS 5

typedef enum {
    CrashEv_None = 0,
    CrashEv_Alert,      /* pause tier newly raised */
    CrashEv_Released,   /* pause tier released after the quiet ticks */
    CrashEv_Alarm       /* fail tier newly latched */
} crash_event_t;

typedef struct {
    int alert;          /* pause tier standing */
    int alarm;          /* fail tier, latched for the run session */
    int clear_ticks;    /* consecutive quiet polls toward release */
    unsigned axes;      /* axis bits of the trip (CRASH_SRC_XH/YH) */
} crash_watch_t;

/* One poll's verdict from the two latched source registers. An abort
 * event latches the alarm for the session; an alert event raises the
 * pause tier and a quiet run of polls releases it. A latched alarm
 * ignores everything after it. */
crash_event_t crash_tick(crash_watch_t *w, unsigned src1, unsigned src2);

/* A new run session: both tiers and the counters cleared. */
void crash_reset(crash_watch_t *w);

/* "x", "y" or "xy" from the recorded axis bits. */
const char *crash_axes_name(unsigned axes);

/* Hardware: the head LIS2HH12 at /dev/i2c-3 addr 0x1e, reached with
 * I2C_SLAVE_FORCE while st_accel stays bound.
 *
 * The engine's calls. open is its claim on the handle: it opens one
 * when none is open (probing WHO_AM_I) and takes over a handle a check
 * opened; the handle is the engine's from then on. arm programs the
 * generators (a threshold of 0 leaves that tier unarmed) and takes the
 * ODR and full scale, ending any check's borrow; poll reads and clears
 * both source registers, re-asserting CTRL1/CTRL4 first if a one-shot
 * read elsewhere put the part back to power-down; disarm zeroes the
 * generators and restores the part's own registers, and is a no-op when
 * the engine's arm does not hold the part; close gives the part back
 * when anyone still holds it or a give-back was cut short (opening the
 * bus once more when a check's return had closed it), as far as the bus
 * answers, ends the claim and every borrow, closes the handle, and
 * refuses every borrow after it (the daemon's shutdown). A give-back stops at the first write
 * the bus loses; until one completes, the next holder keeps the saved
 * registers rather than read the part's. All return 0 on success,
 * -1 on an i2c failure (open: also when the part does not answer as a
 * LIS2HH12; arm and poll: also without the engine's claim or arm). */
int  crash_hw_open(void);
int  crash_hw_arm(int ths_x_alert, int ths_y_alert, int ths_abort);
int  crash_hw_poll(unsigned *src1, unsigned *src2);
void crash_hw_disarm(void);
void crash_hw_close(void);

/* A check's calls. borrow starts its use of the part, opening the
 * handle when none is open: 0, -1 when the part does not answer or the
 * engine has closed the handle, -2 when the part is held (the crash
 * watch armed, or a borrow standing).
 * check_arm and check_poll are arm and poll for the borrow. Every call
 * after borrow is refused (-1, nothing read or written) once the borrow
 * has ended, an armed window having taken the part. return ends the
 * borrow: the generators off and the part's own registers back when the
 * check still holds them, and the handle closed only when the borrow
 * opened it and the engine has not claimed it since. */
int  crash_hw_borrow(void);
int  crash_hw_check_arm(int ths_x_alert, int ths_y_alert, int ths_abort);
int  crash_hw_check_poll(unsigned *src1, unsigned *src2);
/* The Motion check's sample, after its check_arm: x and y as signed
 * counts at the armed full scale, from the running part. The same
 * re-assertion as poll when a one-shot read elsewhere powered the part
 * down, with a short settle so the sample is a fresh one. -1 on an i2c
 * failure. */
int  crash_hw_sample(long *x, long *y);
/* The lens stop finder's listening: the three axes in one burst at the
 * run rate. crash_hw_listen(1) borrows the part (borrow's returns),
 * saves the rate and scale and sets the run values at 2 g;
 * crash_hw_listen(0) is the return, which puts them back. */
int  crash_hw_listen(int on);
int  crash_hw_burst(long *x, long *y, long *z);
void crash_hw_return(void);

/* The Sensors check's watch of the part at rest: begin borrows the part
 * and arms it for the check, poll judges one sample with the tiers
 * (crash_tick: the raw bits alone read as motion on a still head), end
 * returns the borrow. `watched` is true only while the part has been
 * watched the whole time: the borrow and the arm both taken, and no
 * poll refused or failed (a window that takes the part ends it).
 * `events` counts the alert and alarm events judged, and keeps them. */
typedef struct {
    int lent;           /* the borrow stands: end returns it */
    int watched;
    int events;
    crash_watch_t cw;
} crash_check_t;

void crash_check_begin(crash_check_t *c, int ths_x_alert, int ths_y_alert, int ths_abort);
void crash_check_poll(crash_check_t *c);
void crash_check_end(crash_check_t *c);

#endif
