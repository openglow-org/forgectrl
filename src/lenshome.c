/*
 * lenshome.c - forgectrl: lens hall-edge reference
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The lens carriage has one reference on the whole machine: the hall
 * sensor's rising edge, whose focal height the focus card measured. Until
 * the lens sits on it the controller cannot know the focal height, so its
 * Z soft limit pins Z where it stands and every Z move is refused. The
 * hunt runs here, in the motion-verify window: the enclosure is already
 * checked, the 40 V rail is up, the broker holds the pulse device, and no
 * controller has started, so nothing else is driving the machine.
 *
 * The sweep is the factory-path sequence the cloud runner uses (gfhardware
 * ZAxis.home): full steps at the drive current, away from the sensor until
 * it releases, then back toward it until it trips. That second transition
 * is the rising edge. The lens is left standing on it and the controller
 * reads the focal height for it out of lens_hall_edge_z_mm - this file
 * never interprets the edge, it only puts the lens on it.
 *
 * A sweep that runs past MAX_SWEEP_STEPS is a hard fault, not a retry: the
 * lens motor is wedged, the carriage is jammed, or the hall sensor is
 * dead, and a laser whose focal height is a guess does not get to run. The
 * bound also keeps a dead sensor from driving the carriage onto a stop.
 * The caller turns a fault into the motion-fault gate, so no controller
 * spawns and the panel says why.
 */
#define _GNU_SOURCE
#include "lenshome.h"

#include "fflog.h"
#include "paths.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Head sysfs (kernel module). hall_sensor reads 0 while the carriage is
 * over the sensor. z_current 0 = drive, 1 = hold. z_mode 0 = full step,
 * 1 = half. z_enable 0 = energized. cnc/z_step takes the direction, 1 =
 * lens up. */
#define ATTR_HALL     "head/hall_sensor"
#define ATTR_Z_ENABLE "head/z_enable"
#define ATTR_Z_CUR    "head/z_current"
#define ATTR_Z_MODE   "head/z_mode"
#define ATTR_Z_STEP   "cnc/z_step"

#define Z_CUR_DRIVE   "0"
#define Z_CUR_HOLD    "1"
#define Z_MODE_FULL   "0"
#define Z_MODE_HALF   "1"
#define Z_ON          "0"
#define DIR_UP        "1"
#define DIR_DOWN      "0"

/* The carriage's whole travel is a few dozen full steps; several times
 * that bounds a sweep, so a dead sensor cannot step the lens onward
 * indefinitely and a slow head is never failed for being slow. */
#define MAX_SWEEP_STEPS 200
/* The factory path's step cadence. Fast enough that a healthy hunt costs
 * a few seconds, slow enough that the carriage never loses a step. */
#define STEP_DELAY_US   180000

/* Host tests point the head's sysfs at a scratch tree and shorten the
 * cadence; on the machine both are the real ones. */
static const char *sysfs_dir(void)
{
    const char *d = getenv("GF_SYSFS_DIR");
    return d && *d ? d : "/sys/glowforge";
}

static unsigned step_delay_us(void)
{
    const char *d = getenv("GF_LENS_STEP_US");
    if (d && *d) {
        long v = strtol(d, NULL, 10);
        if (v >= 0 && v <= STEP_DELAY_US)
            return (unsigned)v;
    }
    return STEP_DELAY_US;
}

static void attr_path(char *buf, size_t len, const char *attr)
{
    snprintf(buf, len, "%s/%s", sysfs_dir(), attr);
}

static int wr_attr(const char *attr, const char *val)
{
    char path[256];
    attr_path(path, sizeof(path), attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    close(fd);
    return ret;
}

/* 1 = the carriage is over the sensor, 0 = clear, -1 = unreadable (no
 * head: its sysfs group only exists once the head driver has probed). */
static int over_sensor(void)
{
    char path[256], buf[16];
    attr_path(path, sizeof(path), ATTR_HALL);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return buf[0] == '0';
}

/* Step until the sensor reads `want`, at most MAX_SWEEP_STEPS. Returns the
 * steps taken, or -1 if the bound was reached, -2 if the head went away
 * mid-sweep. Toward the sensor is up, away from it is down: the same
 * senses the cloud runner uses. */
static int sweep_until(int want, int *steps_out)
{
    const char *dir = want ? DIR_UP : DIR_DOWN;
    for (int steps = 0; steps <= MAX_SWEEP_STEPS; steps++) {
        int at = over_sensor();
        if (at < 0)
            return -2;
        if (at == want) {
            *steps_out = steps;
            return 0;
        }
        if (steps == MAX_SWEEP_STEPS)
            break;
        if (wr_attr(ATTR_Z_STEP, dir) != 0)
            return -2;
        usleep(step_delay_us());
    }
    return -1;
}

static void rest_motor(void)
{
    wr_attr(ATTR_Z_CUR, Z_CUR_HOLD);
    wr_attr(ATTR_Z_MODE, Z_MODE_HALF);
}

static void marker_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/lens.home", ff_run_dir());
}

void lenshome_clear(void)
{
    char path[256];
    marker_path(path, sizeof(path));
    unlink(path);
}

/* The marker the controller reads at its start: the lens is standing on
 * the hall edge, referenced in this broker hold. The controller turns
 * that into a focal height from its own settings; the counts here are for
 * the log and the panel, not for the controller. */
static void marker_write(int off_steps, int on_steps)
{
    char path[256], tmp[288];
    marker_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fflog(LOG_ERR, "lenshome: cannot write %s", tmp);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "edge %ld %d %d\n", (long)ts.tv_sec, off_steps, on_steps);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) {
        fflog(LOG_ERR, "lenshome: cannot place %s", path);
        unlink(tmp);
    }
}

int lenshome_run(char *detail, size_t dlen)
{
    lenshome_clear();

    char step_path[256];
    attr_path(step_path, sizeof(step_path), ATTR_Z_STEP);
    if (access(step_path, W_OK) != 0) {
        snprintf(detail, dlen, "no lens interface (not this hardware)");
        return -1;
    }
    if (over_sensor() < 0) {
        snprintf(detail, dlen, "no head detected");
        return 0;
    }

    /* Full steps at the drive current: the sweep wants torque and the
     * coarsest step, and the motor goes back to its hold current and the
     * half-step mode a job uses before this returns. */
    wr_attr(ATTR_Z_ENABLE, Z_ON);
    wr_attr(ATTR_Z_CUR, Z_CUR_DRIVE);
    wr_attr(ATTR_Z_MODE, Z_MODE_FULL);

    int off_steps = 0, on_steps = 0;
    int rc = sweep_until(0, &off_steps);
    if (rc == 0)
        rc = sweep_until(1, &on_steps);
    rest_motor();

    if (rc == -2) {
        snprintf(detail, dlen, "the head went away during the hunt");
        return 0;
    }
    if (rc == -1) {
        snprintf(detail, dlen, "no hall edge in %d steps (wedged lens motor, "
                               "jammed carriage, or dead sensor)",
                               MAX_SWEEP_STEPS);
        return 0;
    }

    marker_write(off_steps, on_steps);
    snprintf(detail, dlen, "lens on the hall edge (%d off, %d back)",
             off_steps, on_steps);
    return 1;
}
