/*
 * lenshome_test.c - host unit test for the lens hall-edge reference
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The hunt's failures are the point of this test: a wedged lens motor, a
 * jammed carriage, and a dead hall sensor all have to come out as a hard
 * fault with no marker left behind, and none of the three can be staged
 * on a machine whose hardware works. The head's sysfs is a scratch tree
 * here (GF_SYSFS_DIR) and the step cadence is shortened (GF_LENS_STEP_US),
 * so each case is a file layout rather than a broken machine.
 *
 * The one case with real hardware behind it, the sweep that finds the
 * edge, is staged by flipping the sensor's file from a child process
 * while the sweep runs.
 */
#define _GNU_SOURCE
#include "../src/lenshome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[128];
static int failures;

static void put(const char *rel, const char *val)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        printf("FAIL cannot stage %s\n", path);
        failures++;
        return;
    }
    fputs(val, f);
    fclose(f);
}

static void rm(const char *rel)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    unlink(path);
}

static int marker_present(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/run/lens.home", root);
    return access(path, R_OK) == 0;
}

/* A machine whose head is present and whose sensor answers `hall`. */
static void stage(const char *hall)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/head", root);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/cnc", root);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0755);
    put("cnc/z_step", "");
    put("head/z_enable", "");
    put("head/z_current", "");
    put("head/z_mode", "");
    if (hall)
        put("head/hall_sensor", hall);
    else
        rm("head/hall_sensor");
}

static void expect(const char *what, int got, int want, int want_marker)
{
    int marker = marker_present();
    if (got != want || marker != want_marker) {
        printf("FAIL %s: rc %d (want %d), marker %d (want %d)\n",
               what, got, want, marker, want_marker);
        failures++;
    } else {
        printf("ok   %s: rc %d, marker %d\n", what, got, marker);
    }
}

int main(void)
{
    char detail[96];

    snprintf(root, sizeof(root), "/tmp/lenshome_test.%d", (int)getpid());
    mkdir(root, 0755);
    setenv("GF_SYSFS_DIR", root, 1);
    setenv("GF_LENS_STEP_US", "1000", 1);
    char run[256];
    snprintf(run, sizeof(run), "%s/run", root);
    setenv("GF_RUN_DIR", run, 1);

    /* Not this hardware: no lens interface at all. The machine is not
     * faulted for it, because nothing was tested. */
    stage("1");
    rm("cnc/z_step");
    expect("no lens interface", lenshome_run(detail, sizeof(detail)), -1, 0);

    /* No head: its sysfs group never appeared. A fault, because the lens
     * cannot be referenced and a job's focal height would be a guess. */
    stage(NULL);
    expect("no head", lenshome_run(detail, sizeof(detail)), 0, 0);

    /* A dead sensor stuck clear: the carriage never trips it, so the
     * sweep toward it runs out its bound. */
    stage("1");
    expect("sensor stuck clear", lenshome_run(detail, sizeof(detail)), 0, 0);

    /* A dead sensor stuck tripped, or a carriage that cannot move off
     * it: the sweep away from the sensor runs out its bound. */
    stage("0");
    expect("sensor stuck tripped", lenshome_run(detail, sizeof(detail)), 0, 0);

    /* The healthy sweep. The carriage starts over the sensor, comes off
     * it, and trips it again on the way back: the child stands in for the
     * mechanism, clearing the sensor and then tripping it. */
    stage("0");
    pid_t child = fork();
    if (child == 0) {
        usleep(20000);
        put("head/hall_sensor", "1");
        usleep(20000);
        put("head/hall_sensor", "0");
        _exit(0);
    }
    int rc = lenshome_run(detail, sizeof(detail));
    waitpid(child, NULL, 0);
    expect("edge found", rc, 1, 1);

    /* The reference is only good for the hold that made it. */
    lenshome_clear();
    if (marker_present()) {
        printf("FAIL clear: the marker outlived the hold\n");
        failures++;
    } else {
        printf("ok   clear: the marker is gone\n");
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", root);

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
