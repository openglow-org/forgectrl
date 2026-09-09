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
 * The healthy sweep needs a carriage that moves, and it is driven by the
 * steps the sweep itself writes rather than by a clock: a child watches
 * the step attribute and answers the sensor after the same counts the
 * bench reference gives. Nothing here depends on how fast the machine
 * running the test happens to be, and the sweep cannot run past a
 * transition, because the transition is what its own stepping causes.
 */
#define _GNU_SOURCE
#include "../src/lenshome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The bench reference's counts: the carriage starts on the sensor, comes
 * off it in three full steps, and trips it again three steps later. */
#define STEPS_TO_CLEAR 3
#define STEPS_TO_TRIP  6

static char root[128];
static int failures;

static void path_of(char *buf, size_t len, const char *rel)
{
    snprintf(buf, len, "%s/%s", root, rel);
}

static void put(const char *rel, const char *val)
{
    char path[256];
    path_of(path, sizeof(path), rel);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        printf("FAIL cannot stage %s\n", path);
        failures++;
        return;
    }
    fputs(val, f);
    fclose(f);
}

/* The sensor is rewritten under a running sweep, so it is replaced whole:
 * a truncated file caught mid-write would read as a head that went away. */
static void put_atomic(const char *rel, const char *val)
{
    char path[256], tmp[300];
    path_of(path, sizeof(path), rel);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fputs(val, f);
    fclose(f);
    if (rename(tmp, path) != 0)
        unlink(tmp);
}

static void rm(const char *rel)
{
    char path[256];
    path_of(path, sizeof(path), rel);
    unlink(path);
}

static int marker_present(void)
{
    char path[256];
    path_of(path, sizeof(path), "run/lens.home");
    return access(path, R_OK) == 0;
}

/* A machine whose head is present and whose sensor answers `hall`. */
static void stage(const char *hall)
{
    char dir[256];
    path_of(dir, sizeof(dir), "head");
    mkdir(dir, 0755);
    path_of(dir, sizeof(dir), "cnc");
    mkdir(dir, 0755);
    path_of(dir, sizeof(dir), "run");
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

/* The carriage: every step the sweep writes moves it, and the sensor
 * answers the count. Runs until the sweep has stepped it past the edge. */
static void carriage(void)
{
    char step[256];
    struct stat st;
    struct timespec last = { 0, 0 };
    int seen = 0, steps = 0;

    path_of(step, sizeof(step), "cnc/z_step");
    for (;;) {
        if (stat(step, &st) == 0) {
            if (st.st_mtim.tv_sec != last.tv_sec ||
                st.st_mtim.tv_nsec != last.tv_nsec) {
                if (seen)
                    steps++;
                seen = 1;
                last = st.st_mtim;
                if (steps == STEPS_TO_CLEAR)
                    put_atomic("head/hall_sensor", "1");
                else if (steps == STEPS_TO_TRIP)
                    put_atomic("head/hall_sensor", "0");
            }
        }
        if (steps >= STEPS_TO_TRIP)
            _exit(0);
        usleep(200);
    }
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
    char detail[80];
    char run[256];

    snprintf(root, sizeof(root), "/tmp/lenshome_test.%d", (int)getpid());
    mkdir(root, 0755);
    setenv("GF_SYSFS_DIR", root, 1);
    setenv("GF_LENS_STEP_US", "2000", 1);
    path_of(run, sizeof(run), "run");
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

    /* A dead sensor stuck tripped, or a carriage that cannot move off it:
     * the sweep away from the sensor runs out its bound. */
    stage("0");
    expect("sensor stuck tripped", lenshome_run(detail, sizeof(detail)), 0, 0);

    /* The healthy sweep, against a carriage the sweep's own steps move. */
    stage("0");
    pid_t child = fork();
    if (child == 0)
        carriage();
    int rc = lenshome_run(detail, sizeof(detail));
    kill(child, SIGKILL);
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
