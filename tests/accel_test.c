/*
 * accel_test.c - host unit test for the crash watch: its tiers, and the
 * head accelerometer's bus handle between the engine and the checks
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The tiers, pinned: an IG1 event raises the pause tier once and a
 * quiet run of polls releases it; an IG2 event latches the fail tier
 * for the session whatever comes after; axis bits without IA are noise
 * on either source; a session reset clears both; the axis bits ride
 * along for the log line.
 *
 * The bus handle: the real accel.c, included here over a fake bus that
 * carries a model of the head LIS2HH12. The model keeps the registers;
 * its two interrupt generators latch an impact only on an axis their
 * IG_CFG enables, at a running ODR, with the latch bit set in CTRL7, and
 * a read of a source register clears its latch, as the part does. The
 * engine's calls are made as the cooling engine makes them at each armed
 * window, the checks' as the Sensors, Motion and Lens travel checks make
 * them, and the cases walk:
 *
 *   a check before any armed window, with a window opening during it
 *   an armed window, a check at idle, the next window with a crash
 *   a check inside an armed window, with a crash
 *   the Sensors check's watch, and a window that takes it
 *   the lens finder's listening at idle
 *   a listening that cannot start; a listening's end that loses a write
 *   an arm that loses a write; a give-back that loses one, and what the
 *   next window and the shutdown give back after it
 *   a shutdown while a check holds the part, a borrow after it, and the
 *   next daemon; a check's return cut short before any claim
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

static int fake_bus_open(const char *path, int flags, ...);
static int fake_bus_ioctl(int fd, unsigned long req, ...);
static ssize_t fake_bus_read(int fd, void *buf, size_t n);
static ssize_t fake_bus_write(int fd, const void *buf, size_t n);
static int fake_bus_close(int fd);
static int fake_usleep(unsigned us);
#define open   fake_bus_open
#define ioctl  fake_bus_ioctl
#define read   fake_bus_read
#define write  fake_bus_write
#define close  fake_bus_close
#define usleep fake_usleep
#include "../src/accel.c"
#undef open
#undef ioctl
#undef read
#undef write
#undef close
#undef usleep

/* --- the fake bus and the part on it --------------------------------- */

#define FAKE_FD     1000
#define CTRL1_REST  0x07            /* power-down, the axes on: st_accel's rest */
#define CTRL4_REST  0x04            /* +/-2 g, address auto-increment */
#define CTRL7_LIR1  0x04
#define CTRL7_LIR2  0x08

static unsigned char part[0x40];    /* the head LIS2HH12's registers */
static unsigned char ptr;           /* the address a one-byte write set */
static int fd_open;                 /* the bus handle is open */
static int opens;                   /* opens of the bus over the run */
static int unlocked_io;             /* bus transfers made without accel.c's lock */
static int writes;                  /* register writes over the run */
static int fail_write;              /* this write fails, once (0: none) */
static int transfers;               /* reads and writes of any kind over the run */

/* Every transfer runs under the lock, so the engine's thread and a
 * check's never interleave on the bus: a trylock that takes it means
 * nobody held it. */
static void lock_held(void)
{
    if (pthread_mutex_trylock(&hw_mu) == 0) {
        unlocked_io++;
        pthread_mutex_unlock(&hw_mu);
    }
}

static void part_power_on(void)
{
    memset(part, 0, sizeof(part));
    part[WHO_AM_I] = WHO_AM_I_LIS2HH12;
    part[CTRL1] = CTRL1_REST;
    part[CTRL4] = CTRL4_REST;
}

/* An impact past every threshold on the given axis bits: each generator
 * that enables one of them latches it, while the part samples. */
static void part_impact(unsigned axes)
{
    if (!(part[CTRL1] & 0x70))
        return;                     /* ODR 0: powered down, nothing samples */
    unsigned a1 = part[IG_CFG1] & axes, a2 = part[IG_CFG2] & axes;
    if (a1 && (part[CTRL7] & CTRL7_LIR1))
        part[IG_SRC1] |= (unsigned char)(CRASH_SRC_IA | a1);
    if (a2 && (part[CTRL7] & CTRL7_LIR2))
        part[IG_SRC2] |= (unsigned char)(CRASH_SRC_IA | a2);
}

static int fake_bus_open(const char *path, int flags, ...)
{
    (void)flags;
    lock_held();
    if (strcmp(path, BUS_DEV) != 0 || fd_open) {
        errno = EBUSY;
        return -1;
    }
    fd_open = 1;
    opens++;
    return FAKE_FD;
}

static int fake_bus_ioctl(int fd, unsigned long req, ...)
{
    lock_held();
    return fd == FAKE_FD && fd_open && req == I2C_SLAVE_FORCE ? 0 : -1;
}

static ssize_t fake_bus_write(int fd, const void *buf, size_t n)
{
    const unsigned char *b = buf;
    lock_held();
    transfers++;
    if (fd != FAKE_FD || !fd_open || n < 1) {
        errno = EBADF;
        return -1;
    }
    if (n == 2 && ++writes == fail_write) {
        errno = EIO;                /* a glitch on the bus: this transfer is lost */
        return -1;
    }
    ptr = b[0] & 0x7f;              /* bit 7: auto-increment */
    if (n == 2 && ptr != WHO_AM_I && ptr != IG_SRC1 && ptr != IG_SRC2)
        part[ptr & 0x3f] = b[1];
    return (ssize_t)n;
}

static ssize_t fake_bus_read(int fd, void *buf, size_t n)
{
    unsigned char *b = buf;
    lock_held();
    transfers++;
    if (fd != FAKE_FD || !fd_open) {
        errno = EBADF;
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char r = (unsigned char)((ptr + i) & 0x3f);
        b[i] = part[r];
        if (r == IG_SRC1 || r == IG_SRC2)
            part[r] = 0;            /* the read clears the latch */
    }
    return (ssize_t)n;
}

static int fake_bus_close(int fd)
{
    lock_held();
    if (fd != FAKE_FD || !fd_open) {
        errno = EBADF;
        return -1;
    }
    fd_open = 0;
    return 0;
}

static int fake_usleep(unsigned us)
{
    (void)us;
    return 0;
}

/* --- the cases --------------------------------------------------------- */

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

#define X_ALERT 132
#define Y_ALERT 112
#define ABORT   133

/* The engine's poll, judged by the tiers. */
static crash_event_t engine_poll(crash_watch_t *w)
{
    unsigned s1 = 0, s2 = 0;
    if (crash_hw_poll(&s1, &s2) != 0)
        return (crash_event_t)-1;
    return crash_tick(w, s1, s2);
}

static void tiers(void)
{
    crash_watch_t w;
    crash_reset(&w);

    printf("quiet\n");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_None && !w.alert && !w.alarm,
          "no sources: nothing");
    CHECK(crash_tick(&w, CRASH_SRC_XH, 0) == CrashEv_None,
          "axis bits without IA are noise, not an event");
    CHECK(crash_tick(&w, 0, CRASH_SRC_XH | CRASH_SRC_YH) == CrashEv_None && !w.alarm,
          "on the abort source too: IG2 axis bits without IA raise no alarm");

    printf("alert tier\n");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "IG1 IA raises the alert");
    CHECK(w.alert && w.axes == CRASH_SRC_XH, "and records the axis");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_YH, 0) == CrashEv_None,
          "a standing alert is raised once");
    CHECK(w.axes == CRASH_SRC_YH, "but the axis record follows the signal");
    for (int i = 0; i < CRASH_CLEAR_TICKS - 1; i++)
        CHECK(crash_tick(&w, 0, 0) == CrashEv_None && w.alert,
              "a quiet poll under the count keeps holding");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_Released && !w.alert,
          "the quiet count releases the alert");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "a new event raises it again");
    crash_tick(&w, 0, 0);
    crash_tick(&w, 0, 0);
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_None
          && w.clear_ticks == 0,
          "an event inside the quiet count restarts it");

    printf("fail tier\n");
    crash_reset(&w);
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA | CRASH_SRC_XH | CRASH_SRC_YH)
          == CrashEv_Alarm, "IG2 IA latches the alarm");
    CHECK(w.alarm && w.axes == (CRASH_SRC_XH | CRASH_SRC_YH),
          "with both axes recorded");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_None && w.alarm,
          "quiet polls do not clear it");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_None
          && w.alarm, "an alert event after the alarm changes nothing");
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA) == CrashEv_None,
          "a second abort event is not a second alarm");

    printf("alert then abort\n");
    crash_reset(&w);
    crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0);
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA | CRASH_SRC_YH) == CrashEv_Alarm,
          "an abort during a standing alert still latches the alarm");
    CHECK(w.alarm, "latched");

    printf("session reset\n");
    crash_reset(&w);
    CHECK(!w.alert && !w.alarm && w.clear_ticks == 0 && w.axes == 0,
          "a new run session clears both tiers");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "and the watch works again");

    printf("axis names\n");
    CHECK(crash_axes_name(CRASH_SRC_XH)[0] == 'x', "x alone");
    CHECK(crash_axes_name(CRASH_SRC_YH)[0] == 'y', "y alone");
    CHECK(crash_axes_name(CRASH_SRC_XH | CRASH_SRC_YH)[1] == 'y', "both");
    CHECK(crash_axes_name(0)[0] == '-', "none");
}

static void bus_handle(void)
{
    crash_watch_t w;
    unsigned s1, s2;
    part_power_on();

    printf("the bus handle: a check before any armed window, a window opening during it\n");
    CHECK(crash_hw_borrow() == 0 && fd_open && opens == 1 &&
          crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == 0,
          "a Sensors check before any window opens the handle for its borrow and arms the part");
    part_impact(CRASH_SRC_XH);                      /* the check's own event: a knock, a jog */
    /* A Grbl sender's job holds no lease: its window can open now. */
    CHECK(crash_hw_open() == 0 && crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0 && opens == 1,
          "the window's watch claims that handle and arms");
    crash_reset(&w);
    CHECK(engine_poll(&w) == CrashEv_None,
          "its arm cleared the check's latch: no crash judged on a head that did not crash");
    long x, y, z;
    int before = transfers;
    CHECK(crash_hw_sample(&x, &y) == -1 && crash_hw_burst(&x, &y, &z) == -1 && transfers == before,
          "the borrow ended there: the check's sample and burst are refused with no bus transfer");
    part_impact(CRASH_SRC_XH);
    s1 = s2 = 0;
    CHECK(crash_hw_check_poll(&s1, &s2) == -1 && part[IG_SRC2] != 0,
          "and its poll reads nothing: the latch stays for the watch");
    CHECK(engine_poll(&w) == CrashEv_Alarm, "the watch catches the crash: the fail tier");
    crash_hw_disarm();
    CHECK(part[CTRL1] == CTRL1_REST && part[CTRL4] == CTRL4_REST && part[CTRL7] == 0 &&
          part[IG_CFG1] == 0 && part[IG_CFG2] == 0,
          "the window's disarm gives the part back as it was before the check took it");
    CHECK(crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == -1 && part[IG_CFG2] == 0,
          "the check, still running, cannot take the part back with the window closed");
    crash_hw_return();
    CHECK(fd_open && part[CTRL1] == CTRL1_REST,
          "and its return leaves the handle the engine claimed, and the part, alone");

    printf("the bus handle: an armed window, a check at idle, the next window with a crash\n");
    CHECK(crash_hw_borrow() == 0 && crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == 0,
          "a Sensors or Motion check at idle borrows the part and arms it");
    crash_hw_disarm();
    CHECK(part[IG_CFG2] == (CFG_XHIE | CFG_YHIE),
          "a disarm by the engine, which did not arm, leaves the check's generators");
    crash_hw_check_poll(&s1, &s2);
    crash_hw_return();
    CHECK(fd_open && opens == 1, "its return leaves the engine's handle open");
    /* The engine holds its claim from the first window: the next window
     * arms on it with no new open. */
    CHECK(crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0, "the next window's watch arms");
    part_impact(CRASH_SRC_YH);
    crash_reset(&w);
    CHECK(engine_poll(&w) == CrashEv_Alarm, "and catches a crash: the fail tier");

    printf("the bus handle: a check inside an armed window\n");
    CHECK(crash_hw_borrow() == -2 && crash_hw_listen(1) == -2,
          "a check's borrow and the lens finder's listening are refused while the watch is armed");
    part_impact(CRASH_SRC_XH);
    s1 = s2 = 0;
    CHECK(crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == -1 &&
          crash_hw_check_poll(&s1, &s2) == -1 && part[IG_SRC2] != 0,
          "a check without a borrow neither arms nor reads: the latch stays for the watch");
    crash_hw_return();
    CHECK(fd_open && part[IG_CFG2] == (CFG_XHIE | CFG_YHIE),
          "nor does its return touch the watch's generators or handle");
    crash_reset(&w);
    CHECK(engine_poll(&w) == CrashEv_Alarm, "the watch catches the crash: the fail tier");
    crash_hw_disarm();

    printf("the bus handle: the Sensors check's watch\n");
    crash_check_t c;
    crash_check_begin(&c, X_ALERT, Y_ALERT, ABORT);
    /* A Y bump between the alert threshold (112) and the abort (133):
     * IG1 latches it, IG2 does not. */
    part[IG_SRC1] |= CRASH_SRC_IA | CRASH_SRC_YH;
    crash_check_poll(&c);
    CHECK(c.watched && c.events == 1,
          "at idle it borrows and arms the part, and counts an alert-tier bump");
    part_impact(CRASH_SRC_XH);                      /* past every threshold: the fail tier */
    crash_check_poll(&c);
    CHECK(c.events == 2 && c.cw.alarm, "and a knock past the abort");
    CHECK(crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0, "a window opens during the check");
    crash_check_poll(&c);
    CHECK(!c.watched && c.events == 2,
          "the window took the part: the check says it did not watch, and keeps its events");
    crash_check_end(&c);
    crash_hw_disarm();
    fail_write = writes + 1;                        /* the check's arm loses its first write */
    crash_check_begin(&c, X_ALERT, Y_ALERT, ABORT);
    CHECK(!c.watched, "an arm that fails leaves the check unwatched");
    crash_check_end(&c);
    CHECK(crash_hw_borrow() == 0, "and its end returns the borrow");
    crash_hw_return();
    crash_check_begin(&c, X_ALERT, Y_ALERT, ABORT);
    part[IG_SRC1] |= CRASH_SRC_IA | CRASH_SRC_YH;
    crash_check_poll(&c);
    CHECK(c.watched && c.events == 1,
          "a check begun on a record that saw an alarm starts clean: its bump counts");
    crash_check_end(&c);

    printf("the bus handle: the lens finder's listening at idle\n");
    part[CTRL4] = CTRL4_REST | CTRL4_FS_4G;         /* a one-shot reader left 4 g */
    fail_write = writes + 1;                        /* the listening's 2 g write is lost */
    CHECK(crash_hw_listen(1) == -1 && crash_hw_borrow() == 0,
          "a listening that cannot start fails, and leaves no borrow standing");
    crash_hw_return();
    CHECK(crash_hw_listen(1) == 0 &&
          (part[CTRL4] & CTRL4_FS_MASK) == 0 && part[CTRL1] == CTRL1_RUN,
          "the listening takes 2 g at the run rate");
    CHECK(crash_hw_burst(&x, &y, &z) == 0, "and reads its bursts");
    crash_hw_listen(0);
    CHECK(part[CTRL1] == CTRL1_REST && part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G) && fd_open,
          "its end gives both back and leaves the engine's handle open");
    crash_hw_listen(1);
    fail_write = writes + 1;                        /* the end's first write is lost */
    crash_hw_listen(0);
    crash_hw_listen(1);
    crash_hw_listen(0);
    CHECK(part[CTRL1] == CTRL1_REST && part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G),
          "an end cut short leaves the saved registers standing: the next listening's end restores them");

    printf("the bus handle: an arm that fails partway\n");
    fail_write = writes + 4;        /* past the latch and the alert thresholds */
    CHECK(crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == -1, "a lost write fails the window's arm");
    CHECK(part[CTRL7] == 0 && part[IG_CFG1] == 0 && part[IG_CFG2] == 0 &&
          part[CTRL1] == CTRL1_REST && part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G),
          "the failed arm gives the part back as it was");
    CHECK(crash_hw_borrow() == 0, "and leaves it free: a check after it borrows it");
    crash_hw_return();

    printf("the bus handle: a shutdown while a check holds the part\n");
    CHECK(crash_hw_borrow() == 0 && crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == 0 &&
          part[CTRL1] == CTRL1_RUN, "a check holds the part at its run rate");
    crash_hw_close();
    CHECK(!fd_open && part[CTRL1] == CTRL1_REST && part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G) &&
          part[CTRL7] == 0 && part[IG_CFG1] == 0 && part[IG_CFG2] == 0,
          "the engine's close gives the part back as it was, then closes the handle");
    crash_hw_return();
    int opened = opens;
    CHECK(crash_hw_borrow() == -1 && crash_hw_listen(1) == -1 &&
          crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == -1 && !fd_open && opens == opened &&
          part[CTRL1] == CTRL1_REST && part[IG_CFG2] == 0,
          "after the close, a borrow, a listening and a check's arm are refused: the bus stays shut");

    printf("the bus handle: the next daemon, its tick's claim between a check's borrow and return\n");
    closed = dirty = 0;                             /* a new process */
    part_power_on();                                /* the next boot */
    CHECK(crash_hw_borrow() == 0 && fd_open && crash_hw_open() == 0,
          "a check's borrow opens the handle, and the window's tick claims it");
    crash_hw_return();
    CHECK(fd_open && crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0,
          "the check's return leaves the claimed handle open: the tick's arm takes it");

    printf("the bus handle: a give-back on a bus that loses a write\n");
    int w0 = writes;
    fail_write = writes + 1;
    crash_hw_disarm();
    CHECK(writes == w0 + 1 && part[CTRL1] == CTRL1_RUN,
          "the first lost write ends the give-back: nothing after it, the part left running");
    CHECK(crash_hw_open() == 0 && crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0,
          "the next window arms");
    crash_hw_disarm();
    CHECK(part[CTRL1] == CTRL1_REST && part[CTRL4] == CTRL4_REST && part[CTRL7] == 0 &&
          part[IG_CFG1] == 0 && part[IG_CFG2] == 0,
          "and its whole disarm gives back the part's own registers, not the run ones left behind");
    part[CTRL4] = CTRL4_REST | CTRL4_FS_4G;         /* a one-shot reader set 4 g at idle */
    CHECK(crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0, "another window arms");
    crash_hw_disarm();
    CHECK(part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G),
          "a whole give-back ended that: the window read the part's own scale afresh");
    CHECK(crash_hw_arm(X_ALERT, Y_ALERT, ABORT) == 0, "and another arms");
    fail_write = writes + 1;
    crash_hw_disarm();

    crash_hw_close();
    CHECK(!fd_open && part[CTRL1] == CTRL1_REST && part[CTRL4] == (CTRL4_REST | CTRL4_FS_4G) &&
          part[CTRL7] == 0 && part[IG_CFG1] == 0 && part[IG_CFG2] == 0,
          "a give-back cut short is finished by the engine's close at the shutdown");

    printf("the bus handle: a check's return cut short before any claim, then the shutdown\n");
    closed = dirty = 0;                             /* a new process */
    part_power_on();                                /* the next boot */
    CHECK(crash_hw_borrow() == 0 && fd_open && crash_hw_check_arm(X_ALERT, Y_ALERT, ABORT) == 0,
          "a check before any window opens the handle and arms the part");
    fail_write = writes + 1;
    crash_hw_return();
    CHECK(!fd_open && part[CTRL1] == CTRL1_RUN,
          "its return loses a write and closes the handle it opened: the part left running");
    crash_hw_close();
    CHECK(!fd_open && part[CTRL1] == CTRL1_REST && part[CTRL4] == CTRL4_REST &&
          part[CTRL7] == 0 && part[IG_CFG1] == 0 && part[IG_CFG2] == 0,
          "the engine's close opens the bus once more and finishes the give-back");
    CHECK(unlocked_io == 0, "every bus transfer ran under the lock");
}

int main(void)
{
    tiers();
    bus_handle();
    printf("%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
