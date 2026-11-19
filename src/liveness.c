/*
 * liveness.c - forgectrl: motion-liveness probe
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The DRV8825 stepper drivers on the factory board do not tolerate
 * glitches on the 40 V motor rail: a glitch can leave them in an
 * unserviceable state in which SDMA playback and the position counters
 * run normally while the motors produce nothing. The drivers' reset
 * lines are strapped (no kernel pin), their fault lines do not flag
 * the state, and no counter or register distinguishes it - the ONLY
 * software-visible truth is the head accelerometer, which physically
 * rides the gantry.
 *
 * The probe: write a small X move to the pulse ring through the
 * broker's fd - RIGHT (+X) first, then back, always: a cable lives at
 * the end of LEFT travel and must never be crushed - run it, and
 * sample the head accelerometer through the window. Real motion shows
 * start/stop transients and travel vibration (bench-characterized:
 * peak-to-peak >= ~1000 counts on X during an identical commanded
 * move); wedged drivers show the resting noise floor (p2p <= ~210).
 * The thresholds sit >= 2x from both measured sides.
 *
 * The laser latch stays locked (the probe never touches it), the move
 * is +/-15 mm relative at ~10 mm/s, and the position counters return
 * to their starting values (equal steps out and back). The kernel is
 * left with the program cleared; the controller that starts afterwards
 * reconfigures everything at its own init as usual.
 */
#define _GNU_SOURCE
#include "liveness.h"
#include "testhook.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* EV_SW bits (device tree): 3 = doors (combined; bit SET = closed),
 * 5 = remote interlock (bit SET = open). Same senses as the
 * controller's switch map. The probe moves the gantry, so it must
 * never run with an opening the operator could reach into. */
#define SWITCH_DEV       "/dev/input/event0"
#define SW_BIT_DOORS     3
#define SW_BIT_INTERLOCK 5

/* The same test hook as status.c: GF_SWITCH_DEV lets a host test point
 * the read at a missing or a wrong device, and only a host-test run
 * (testhook.h): on a machine the gate's input is the switch device
 * whatever the environment says, the fail-closed direction. */
static const char *switch_dev(void)
{
    const char *d = ff_test_hook("GF_SWITCH_DEV");
    return d ? d : SWITCH_DEV;
}

/* And the sysfs tree: the test hook GF_SYSFS_ROOT moves it in a
 * host-test run, as status.c reads it (it ends with '/'); else GF_SYSFS
 * stands. */
#ifndef GF_SYSFS
#define GF_SYSFS "/sys/glowforge/"
#endif
static const char *sysfs_root(void)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    return r ? r : GF_SYSFS;
}

int liveness_enclosure_classify(unsigned sw, char *why, size_t wlen)
{
    int lid = !(sw & (1u << SW_BIT_DOORS));
    int interlock = (sw & (1u << SW_BIT_INTERLOCK)) != 0;
    if (!lid && !interlock) {
        if (wlen)
            why[0] = '\0';
        return 0;
    }
    snprintf(why, wlen, "%s", lid && interlock ? "the lid and the interlock are open"
                              : lid ? "the lid is open" : "the interlock is open");
    return 1;
}

/* The switch word from a file, for a host start: the test hook
 * GF_SWITCH_WORD_FILE names a file holding the word as a C integer (8, or
 * 0x8), and only when it is set in a host-test run (testhook.h) is the
 * device left unread; on a machine the gate reads the switch device
 * whatever the environment says. A file that cannot be
 * read, or holds anything else, is a failed read. The file is named by
 * an absolute path; any other form is a failed read. */
static int switch_word_file(const char *path, unsigned *sw)
{
    char buf[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *end;
    unsigned long v = strtoul(buf, &end, 0);
    while (*end == '\n' || *end == ' ')
        end++;
    if (buf[0] < '0' || buf[0] > '9' || *end)
        return -1;
    *sw = (unsigned)v;
    return 0;
}

/* Fails closed: a switch device that cannot be read is no license to
 * drive the gantry with an opening the operator could reach into. */
int liveness_enclosure(char *why, size_t wlen)
{
    unsigned word = 0;
    int ok;
    const char *wf = ff_test_hook("GF_SWITCH_WORD_FILE");
    if (wf) {
        ok = wf[0] == '/' && switch_word_file(wf, &word) == 0;
    } else {
        uint8_t sw[2] = { 0 };
        int fd = open(switch_dev(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        ok = fd >= 0 && ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
        if (fd >= 0)
            close(fd);
        word = sw[0];
    }
    if (!ok) {
        snprintf(why, wlen, "the lid switches cannot be read");
        return -1;
    }
    return liveness_enclosure_classify(word, why, wlen);
}

/* Head accelerometer: iio device on i2c-3 addr 0x1e (glowforge.dts
 * head-accel). Resolved by bus path, never by iio index - probe order
 * is not guaranteed. */
#define HEAD_ACCEL_GLOB "3-001e"

/* Move geometry. 53.333 microsteps/mm (x8 microstepping, the mode the
 * probe configures). */
#define PROBE_MM        15
#define PROBE_STEPS     (PROBE_MM * 160 / 3)    /* 800 microsteps */
#define STEP_FREQ_HZ    10000
#define TICKS_PER_STEP  19                      /* ~10.5 mm/s */
#define LEAD_TICKS      400

/* Verdict thresholds. Bench-characterized on the identical move: a
 * live head reads p2p >= 1040 on X/Y (typically 1800-2900), a dead one
 * <= 250 - but a rail-on or the run-current step just before the move
 * can add a jolt of a few hundred counts, so the moving threshold sits
 * well above that band and the current step settles before sampling. */
#define P2P_MOVING      800
#define P2P_DEAD        250
#define CURRENT_SETTLE_US 300000

/* Pulse-byte bits (kernel feeder contract). */
#define B_X_STEP        0x01
#define B_X_DIR         0x02    /* set = negative X */

/* 0, or -1 with errno saying why. */
static int wr_attr(const char *attr, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    int err = errno;
    close(fd);
    errno = err;
    return ret;
}

/* A probe setup write: -1 with detail naming the attribute and the
 * error when it does not land. */
static int setup_wr(const char *attr, const char *val, char *detail, size_t dlen)
{
    if (wr_attr(attr, val) == 0)
        return 0;
    snprintf(detail, dlen, "cannot write %s: %s", attr, strerror(errno));
    return -1;
}

static int rd_attr(const char *attr, char *buf, size_t len)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The iio device directory: the test hook GF_IIO_ROOT moves it in a
 * host-test run (testhook.h), so a host test can stand in for the head
 * accelerometer; on a machine it is always this one. */
static const char *iio_root(void)
{
    const char *r = ff_test_hook("GF_IIO_ROOT");
    return r ? r : "/sys/bus/iio/devices";
}

/* Find the head accel's iio sysfs directory by its i2c bus address. */
static int head_accel_path(char *buf, size_t len)
{
    for (int i = 0; i < 8; i++) {
        char link[128], resolved[256];
        if (snprintf(link, sizeof(link), "%s/iio:device%d", iio_root(), i) >= (int)sizeof(link))
            return -1;
        ssize_t n = readlink(link, resolved, sizeof(resolved) - 1);
        if (n < 0)
            continue;
        resolved[n] = '\0';
        if (strstr(resolved, HEAD_ACCEL_GLOB)) {
            snprintf(buf, len, "%s", link);
            return 0;
        }
    }
    return -1;
}

static int accel_read(const char *dir, const char *axis, long *out)
{
    char path[160], buf[24];
    snprintf(path, sizeof(path), "%s/in_accel_%s_raw", dir, axis);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    *out = strtol(buf, NULL, 10);
    return 0;
}

int liveness_accel_read(long *x, long *y)
{
    static char dir[128];
    if (!dir[0] && head_accel_path(dir, sizeof(dir)) != 0)
        return -1;
    if (accel_read(dir, "x", x) != 0 || accel_read(dir, "y", y) != 0) {
        dir[0] = '\0';          /* re-resolve next time */
        return -1;
    }
    return 0;
}

int liveness_probe(int pulse_fd, char *detail, size_t dlen, int (*stop)(void))
{
    char accel[128], st[24];

    if (head_accel_path(accel, sizeof(accel)) != 0) {
        snprintf(detail, dlen, "head accelerometer not found");
        return -1;
    }
    if (rd_attr("cnc/state", st, sizeof(st)) != 0) {
        snprintf(detail, dlen, "kernel state unreadable");
        return -4;
    }
    if (strcmp(st, "idle")) {
        /* The last program is still playing (a controller that died
         * mid-move, a ramp): the probe waits for the kernel rather than
         * writing over it, and rather than skipping and starting a
         * controller unverified. */
        snprintf(detail, dlen, "kernel not idle (%s) - the probe waits", st);
        return -3;
    }
    char why[64];
    if (liveness_enclosure(why, sizeof(why)) != 0) {
        /* The enclosure opened between the supervisor's own check and
         * the move, or the switches cannot be read: do not drive the
         * gantry. The supervisor waits for the enclosure and runs the
         * probe again. */
        snprintf(detail, dlen, "%s - motion probe skipped", why);
        return -2;
    }

    /* Configure just enough of the machine for a clean X move: no axis
     * masked (a leftover motor_lock would read as a dead driver), x8
     * microstep mode, slow decay, run current on X. The controller
     * re-applies its full config at its own start. The current step
     * jolts the head; let it settle before the accelerometer is read.
     * Every write is checked: one that does not land would make a live
     * gantry read as dead (a motor still masked, the X motor at its hold
     * current), so it is a fault of its own, named, and nothing moves. */
    if (setup_wr("cnc/motor_lock", "0", detail, dlen) || setup_wr("cnc/x_mode", "8", detail, dlen) ||
        setup_wr("cnc/x_decay", "1", detail, dlen) || setup_wr("pic/x_step_current", "135", detail, dlen))
        goto setup_failed;
    usleep(CURRENT_SETTLE_US);

    char freq[16];
    snprintf(freq, sizeof(freq), "%d", STEP_FREQ_HZ);
    if (setup_wr("cnc/step_freq", freq, detail, dlen))
        goto setup_failed;
    if (lseek(pulse_fd, 1, SEEK_SET) < 0) {     /* clear program + byte counters */
        snprintf(detail, dlen, "cannot clear the pulse program: %s", strerror(errno));
        goto setup_failed;
    }
    if (setup_wr("cnc/streaming", "0", detail, dlen))      /* complete program: EOD = done */
        goto setup_failed;

    /* The pattern: lead-in, +X out, pause, -X back, tail. One byte per
     * tick; a set X_STEP bit is one microstep that tick. */
    size_t total = LEAD_TICKS + PROBE_STEPS * TICKS_PER_STEP + LEAD_TICKS
                   + PROBE_STEPS * TICKS_PER_STEP + LEAD_TICKS;
    uint8_t *pat = calloc(1, total);
    if (!pat) {
        snprintf(detail, dlen, "out of memory");
        goto setup_failed;
    }
    size_t off = LEAD_TICKS;
    for (int i = 0; i < PROBE_STEPS; i++, off += TICKS_PER_STEP)
        pat[off] = B_X_STEP;                    /* +X: DIR clear */
    off += LEAD_TICKS;
    for (int i = 0; i < PROBE_STEPS; i++, off += TICKS_PER_STEP)
        pat[off] = B_X_STEP | B_X_DIR;          /* back: -X */

    ssize_t wr = write(pulse_fd, pat, total);
    free(pat);
    if (wr != (ssize_t)total) {
        snprintf(detail, dlen, "pulse write failed (%zd of %zu)", wr, total);
        lseek(pulse_fd, 1, SEEK_SET);
        goto setup_failed;
    }

    /* The last look before the move: the kernel is idle, so a stop the
     * lever wrote during the settle was lost, and an enclosure that opened
     * since the check above has not been read. Either one, and the run
     * does not start. */
    if (stop && stop()) {
        snprintf(detail, dlen, "the probe was ended before its move");
        lseek(pulse_fd, 1, SEEK_SET);
        wr_attr("pic/x_step_current", "33");
        return -6;
    }
    if (liveness_enclosure(why, sizeof(why)) != 0) {
        snprintf(detail, dlen, "%s - motion probe skipped", why);
        lseek(pulse_fd, 1, SEEK_SET);
        wr_attr("pic/x_step_current", "33");
        return -2;
    }

    if (wr_attr("cnc/run", "1") != 0) {
        snprintf(detail, dlen, "cannot start the probe run: %s", strerror(errno));
        lseek(pulse_fd, 1, SEEK_SET);
        goto setup_failed;
    }

    /* Sample the head accel until the run completes (~3.5 s). The raw
     * sysfs reads are slow (~150 ms); that still lands ~10 samples in
     * the window, plenty for a peak-to-peak verdict. The enclosure is
     * read with every sample: a lid or the interlock opening under the
     * move, or switches that stop answering, stop it (the kernel's
     * controlled deceleration), and the probe answers -5: the head stands
     * where the stop caught it. */
    long minx = 0, maxx = 0, miny = 0, maxy = 0;
    int have = 0, opened = 0;
    double deadline = wall_s() + 12.0;
    while (wall_s() < deadline) {
        if (!opened && liveness_enclosure(why, sizeof(why)) != 0) {
            opened = 1;
            wr_attr("cnc/stop", "1");
        }
        if (opened) {
            if (rd_attr("cnc/state", st, sizeof(st)) == 0 && strcmp(st, "idle") == 0)
                break;      /* the stop has ended the run */
            usleep(20000);
            continue;
        }
        long x, y;
        if (accel_read(accel, "x", &x) == 0 && accel_read(accel, "y", &y) == 0) {
            if (!have) {
                minx = maxx = x;
                miny = maxy = y;
                have = 1;
            } else {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        if (rd_attr("cnc/state", st, sizeof(st)) == 0 && strcmp(st, "idle") == 0
            && wall_s() > deadline - 10.0)
            break;      /* run finished (state back to idle) */
    }

    /* Stand down: hold current, program cleared. Position counters are
     * net unchanged (equal steps out and back). */
    int down = setup_wr("pic/x_step_current", "33", detail, dlen);
    lseek(pulse_fd, 1, SEEK_SET);
    if (down)
        return -4;
    if (opened) {
        snprintf(detail, dlen, "%s - the probe's move was stopped", why);
        return -5;
    }

    long p2px = maxx - minx, p2py = maxy - miny;
    long p2p = p2px > p2py ? p2px : p2py;
    if (!have) {
        snprintf(detail, dlen, "no head accelerometer sample during the probe's move");
        return -4;
    }
    snprintf(detail, dlen, "head accel p2p x=%ld y=%ld (moving>=%d dead<=%d)",
             p2px, p2py, P2P_MOVING, P2P_DEAD);
    return p2p >= P2P_MOVING ? 1 : 0;

setup_failed:
    /* Nothing moved; the X motor goes back to its hold current, as far as
     * the PIC takes it. */
    wr_attr("pic/x_step_current", "33");
    return -4;
}
