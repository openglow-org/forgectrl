/*
 * accel.c - forgectrl: head-accelerometer crash watch
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "accel.h"

#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---------------------------------------------------- pure tier logic */

crash_event_t crash_tick(crash_watch_t *w, unsigned src1, unsigned src2)
{
    if (w->alarm)
        return CrashEv_None;
    if (src2 & CRASH_SRC_IA) {
        w->alarm = 1;
        w->axes = src2 & (CRASH_SRC_XH | CRASH_SRC_YH);
        return CrashEv_Alarm;
    }
    if (src1 & CRASH_SRC_IA) {
        w->axes = src1 & (CRASH_SRC_XH | CRASH_SRC_YH);
        w->clear_ticks = 0;
        if (!w->alert) {
            w->alert = 1;
            return CrashEv_Alert;
        }
        return CrashEv_None;
    }
    if (w->alert && ++w->clear_ticks >= CRASH_CLEAR_TICKS) {
        w->alert = 0;
        w->clear_ticks = 0;
        return CrashEv_Released;
    }
    return CrashEv_None;
}

void crash_reset(crash_watch_t *w)
{
    w->alert = 0;
    w->alarm = 0;
    w->clear_ticks = 0;
    w->axes = 0;
}

const char *crash_axes_name(unsigned axes)
{
    switch (axes & (CRASH_SRC_XH | CRASH_SRC_YH)) {
    case CRASH_SRC_XH:                return "x";
    case CRASH_SRC_YH:                return "y";
    case CRASH_SRC_XH | CRASH_SRC_YH: return "xy";
    default:                          return "-";
    }
}

/* -------------------------------------------------------------- i2c */

#define BUS_DEV          "/dev/i2c-3"
#define ADDR             0x1e
#define I2C_SLAVE_FORCE  0x0706    /* st_accel stays bound; force the addr */

#define WHO_AM_I         0x0F
#define WHO_AM_I_LIS2HH12 0x41
#define CTRL1            0x20      /* ODR [6:4], BDU, XYZ enables */
#define CTRL4            0x23      /* FS [5:4]: 00 = 2 g, 10 = 4 g */
#define CTRL7            0x26      /* LIR2 bit 3, LIR1 bit 2 */
#define OUT_X_L          0x28      /* BDU holds the pair until both are read */
#define OUT_X_H          0x29
#define OUT_Y_L          0x2A
#define OUT_Y_H          0x2B
#define IG_CFG1          0x30
#define IG_SRC1          0x31
#define IG_THS_X1        0x32
#define IG_THS_Y1        0x33
#define IG_THS_Z1        0x34
#define IG_DUR1          0x35
#define IG_CFG2          0x36
#define IG_SRC2          0x37
#define IG_THS2          0x38
#define IG_DUR2          0x39

#define CTRL1_RUN        0x6F      /* 800 Hz, BDU, XYZ on */
#define CTRL4_FS_MASK    0x30
#define CTRL4_FS_4G      0x20      /* the factory run full scale */
#define CTRL7_LIR        0x0C      /* LIR2 | LIR1: latch both sources */
#define CFG_XHIE         0x02
#define CFG_YHIE         0x08

/* Who holds the part's registers: the engine's arm, or a check's arm or
 * listening. */
enum { HOLD_NONE, HOLD_ENGINE, HOLD_CHECK };

/* Everything below hw_mu is read and written under it (accel.h). */
static pthread_mutex_t hw_mu = PTHREAD_MUTEX_INITIALIZER;
static int i2c_fd = -1;
static int engine_claim;            /* the engine holds the handle */
static int borrow;                  /* a check's borrow stands */
static int borrow_opened;           /* the borrow opened the handle */
static int holder = HOLD_NONE;
static int gen_on;                  /* the holder programmed the generators */
/* The part's own rate, scale and latch settings, read by the first to
 * take the registers: a holder that takes them over from another keeps
 * them, so the last give-back restores what the part had. A give-back
 * the bus cut short leaves them standing too (dirty): the part still
 * holds a run configuration, so the next holder must not read it as the
 * part's own, and the next give-back is a whole one. */
static unsigned char saved_ctrl1, saved_ctrl4, saved_ctrl7;
static int dirty;
static int closed;                  /* the engine closed the handle: no new borrow */
static unsigned char run_ctrl4;

static int rd(unsigned char reg, unsigned char *val)
{
    if (write(i2c_fd, &reg, 1) != 1 || read(i2c_fd, val, 1) != 1)
        return -1;
    return 0;
}

static int wr(unsigned char reg, unsigned char val)
{
    unsigned char buf[2] = { reg, val };
    return write(i2c_fd, buf, 2) == 2 ? 0 : -1;
}

static int bus_open(void)
{
    unsigned char who;
    i2c_fd = open(BUS_DEV, O_RDWR | O_CLOEXEC);
    if (i2c_fd < 0)
        return -1;
    if (ioctl(i2c_fd, I2C_SLAVE_FORCE, ADDR) < 0 ||
        rd(WHO_AM_I, &who) != 0 || who != WHO_AM_I_LIS2HH12) {
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }
    return 0;
}

static int take(void)
{
    if (holder != HOLD_NONE || dirty)
        return 0;
    if (rd(CTRL1, &saved_ctrl1) || rd(CTRL4, &saved_ctrl4) ||
        rd(CTRL7, &saved_ctrl7))
        return -1;
    return 0;
}

/* The generators off (when the holder programmed them, or a give-back
 * before this one was cut short) and the part's own registers back. The
 * first write the bus loses ends it: a bus that failed once gains
 * nothing from more, and each failed transfer can take the adapter's
 * whole timeout under the lock. A give-back cut short leaves dirty set
 * until one completes. */
static void give_back(void)
{
    if (gen_on || dirty)
        dirty = wr(IG_CFG1, 0) || wr(IG_CFG2, 0) || wr(CTRL7, saved_ctrl7) ||
                wr(CTRL4, saved_ctrl4) || wr(CTRL1, saved_ctrl1);
    else
        dirty = wr(CTRL4, saved_ctrl4) || wr(CTRL1, saved_ctrl1);
    holder = HOLD_NONE;
    gen_on = 0;
}

static int arm_locked(int who, int ths_x_alert, int ths_y_alert, int ths_abort)
{
    unsigned char cfg1 = 0, cfg2 = 0, src;
    if (take())
        return -1;
    holder = who;
    gen_on = 1;
    run_ctrl4 = (unsigned char)((saved_ctrl4 & ~CTRL4_FS_MASK) | CTRL4_FS_4G);
    if (ths_x_alert > 0)
        cfg1 |= CFG_XHIE;
    if (ths_y_alert > 0)
        cfg1 |= CFG_YHIE;
    if (ths_abort > 0)
        cfg2 |= CFG_XHIE | CFG_YHIE;
    /* Thresholds and latches first, the axis enables last, a stale
     * latch cleared before the first poll judges anything. */
    if (wr(CTRL7, saved_ctrl7 | CTRL7_LIR) ||
        wr(IG_THS_X1, ths_x_alert > 0 ? ths_x_alert & 0xff : 0) ||
        wr(IG_THS_Y1, ths_y_alert > 0 ? ths_y_alert & 0xff : 0) ||
        wr(IG_THS_Z1, 0) || wr(IG_DUR1, 0) ||
        wr(IG_THS2, ths_abort > 0 ? ths_abort & 0xff : 0) ||
        wr(IG_DUR2, 0) ||
        wr(CTRL4, run_ctrl4) || wr(CTRL1, CTRL1_RUN) ||
        wr(IG_CFG1, cfg1) || wr(IG_CFG2, cfg2) ||
        rd(IG_SRC1, &src) || rd(IG_SRC2, &src)) {
        give_back();
        return -1;
    }
    return 0;
}

static int poll_locked(unsigned *src1, unsigned *src2)
{
    unsigned char c1, s1, s2;
    /* A one-shot read elsewhere (st_accel) powers the part back down;
     * the generators only sample at a running ODR, so re-assert it.
     * The tick that finds it stomped judges nothing it missed - the
     * window costs at most a poll period. */
    if (rd(CTRL1, &c1))
        return -1;
    if (c1 != CTRL1_RUN && (wr(CTRL4, run_ctrl4) || wr(CTRL1, CTRL1_RUN)))
        return -1;
    if (rd(IG_SRC1, &s1) || rd(IG_SRC2, &s2))
        return -1;
    *src1 = s1;
    *src2 = s2;
    return 0;
}

/* ------------------------------------------------- the engine's calls */

int crash_hw_open(void)
{
    pthread_mutex_lock(&hw_mu);
    int rc = i2c_fd >= 0 ? 0 : bus_open();
    if (rc == 0)
        engine_claim = 1;
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

int crash_hw_arm(int ths_x_alert, int ths_y_alert, int ths_abort)
{
    int rc = -1;
    pthread_mutex_lock(&hw_mu);
    if (engine_claim && i2c_fd >= 0) {
        /* The watch takes the part from a check that holds it: the
         * borrow ends here, and the check's later calls touch nothing. */
        borrow = 0;
        borrow_opened = 0;
        rc = arm_locked(HOLD_ENGINE, ths_x_alert, ths_y_alert, ths_abort);
    }
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

int crash_hw_poll(unsigned *src1, unsigned *src2)
{
    pthread_mutex_lock(&hw_mu);
    int rc = holder == HOLD_ENGINE ? poll_locked(src1, src2) : -1;
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

void crash_hw_disarm(void)
{
    pthread_mutex_lock(&hw_mu);
    if (holder == HOLD_ENGINE)
        give_back();
    pthread_mutex_unlock(&hw_mu);
}

void crash_hw_close(void)
{
    pthread_mutex_lock(&hw_mu);
    /* A give-back cut short whose handle is already closed (a check's
     * return before the engine's first claim) is finished here too: the
     * bus is opened once more for it. */
    if (i2c_fd < 0 && dirty)
        (void)bus_open();
    if (i2c_fd >= 0) {
        /* A check still holding the part at the shutdown, or a give-back
         * the bus cut short, is given back here, so the next daemon's
         * first holder saves the part's own registers, not a run rate and
         * scale. */
        if (holder != HOLD_NONE || dirty)
            give_back();
        close(i2c_fd);
        i2c_fd = -1;
    }
    closed = 1;
    engine_claim = 0;
    borrow = 0;
    borrow_opened = 0;
    holder = HOLD_NONE;
    gen_on = 0;
    pthread_mutex_unlock(&hw_mu);
}

/* --------------------------------------------------- a check's calls */

static int borrow_locked(void)
{
    /* After the engine's close at the shutdown nothing reopens the bus: a
     * check that reaches its borrow in the shutdown's last seconds would
     * leave the part armed past the process. */
    if (closed)
        return -1;
    if (holder == HOLD_ENGINE || borrow)
        return -2;
    borrow_opened = 0;
    if (i2c_fd < 0) {
        if (bus_open() != 0)
            return -1;
        borrow_opened = 1;
    }
    borrow = 1;
    return 0;
}

static void return_locked(void)
{
    if (!borrow)
        return;
    if (holder == HOLD_CHECK)
        give_back();
    if (borrow_opened && !engine_claim && i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
    borrow = 0;
    borrow_opened = 0;
}

/* The borrow stands and its arm or listening holds the part. */
static int check_holds(void)
{
    return borrow && holder == HOLD_CHECK;
}

int crash_hw_borrow(void)
{
    pthread_mutex_lock(&hw_mu);
    int rc = borrow_locked();
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

void crash_hw_return(void)
{
    pthread_mutex_lock(&hw_mu);
    return_locked();
    pthread_mutex_unlock(&hw_mu);
}

int crash_hw_check_arm(int ths_x_alert, int ths_y_alert, int ths_abort)
{
    pthread_mutex_lock(&hw_mu);
    int rc = borrow && holder == HOLD_NONE
           ? arm_locked(HOLD_CHECK, ths_x_alert, ths_y_alert, ths_abort) : -1;
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

int crash_hw_check_poll(unsigned *src1, unsigned *src2)
{
    pthread_mutex_lock(&hw_mu);
    int rc = check_holds() ? poll_locked(src1, src2) : -1;
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

int crash_hw_sample(long *x, long *y)
{
    unsigned char c1, xl, xh, yl, yh;
    int rc = -1;
    pthread_mutex_lock(&hw_mu);
    if (!check_holds() || rd(CTRL1, &c1))
        goto out;
    if (c1 != CTRL1_RUN) {
        if (wr(CTRL4, run_ctrl4) || wr(CTRL1, CTRL1_RUN))
            goto out;
        usleep(20000);          /* a few ODR periods: the first outputs settle */
    }
    if (rd(OUT_X_L, &xl) || rd(OUT_X_H, &xh) || rd(OUT_Y_L, &yl) || rd(OUT_Y_H, &yh))
        goto out;
    *x = (short)(((unsigned)xh << 8) | xl);
    *y = (short)(((unsigned)yh << 8) | yl);
    rc = 0;
out:
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

int crash_hw_listen(int on)
{
    if (!on) {
        crash_hw_return();
        return 0;
    }
    pthread_mutex_lock(&hw_mu);
    int rc = borrow_locked();
    if (rc == 0) {
        /* The 2 g scale: the lens finder's thresholds were set on it
         * (the crash watch's 4 g halves every reading). */
        if (take() == 0) {
            holder = HOLD_CHECK;
            gen_on = 0;
        }
        if (holder != HOLD_CHECK ||
            wr(CTRL4, (unsigned char)(saved_ctrl4 & ~CTRL4_FS_MASK)) ||
            wr(CTRL1, CTRL1_RUN)) {
            return_locked();
            rc = -1;
        }
    }
    pthread_mutex_unlock(&hw_mu);
    if (rc == 0)
        usleep(20000);          /* a few ODR periods: the first outputs settle */
    return rc;
}

int crash_hw_burst(long *x, long *y, long *z)
{
    unsigned char reg = OUT_X_L | 0x80, b[6];   /* auto-increment through OUT_Z_H */
    int rc = -1;
    pthread_mutex_lock(&hw_mu);
    if (check_holds() && write(i2c_fd, &reg, 1) == 1 && read(i2c_fd, b, 6) == 6) {
        *x = (short)(((unsigned)b[1] << 8) | b[0]);
        *y = (short)(((unsigned)b[3] << 8) | b[2]);
        *z = (short)(((unsigned)b[5] << 8) | b[4]);
        rc = 0;
    }
    pthread_mutex_unlock(&hw_mu);
    return rc;
}

/* ------------------------------------- the Sensors check's watch */

void crash_check_begin(crash_check_t *c, int ths_x_alert, int ths_y_alert, int ths_abort)
{
    crash_reset(&c->cw);
    c->events = 0;
    c->lent = crash_hw_borrow() == 0;
    c->watched = c->lent && crash_hw_check_arm(ths_x_alert, ths_y_alert, ths_abort) == 0;
}

void crash_check_poll(crash_check_t *c)
{
    unsigned s1 = 0, s2 = 0;
    if (!c->watched)
        return;
    if (crash_hw_check_poll(&s1, &s2) != 0) {
        c->watched = 0;         /* a window took the part, or the bus failed */
        return;
    }
    crash_event_t e = crash_tick(&c->cw, s1, s2);
    if (e == CrashEv_Alert || e == CrashEv_Alarm)
        c->events++;
}

void crash_check_end(crash_check_t *c)
{
    if (c->lent)
        crash_hw_return();
    c->lent = 0;
}
