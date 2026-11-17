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
 *
 * The failing sweeps count their steps: the step attribute is a pipe the
 * test holds open, so every write the sweep makes is a byte the test
 * reads back after it, however fast or slow the host. A stuck sensor
 * costs exactly the bound, an old marker does not survive a failed hunt,
 * and a hunt its stop input ends leaves no marker and the motor at rest.
 *
 * The healthy hunt writes the lens motor's attributes alone: X's and Y's
 * step currents, the stepper enable and disable, and the motor lock are
 * staged as sentinels and read back as staged.
 */
#define _GNU_SOURCE
#include "../src/lenshome.h"

#include <fcntl.h>
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
/* The sweep's bound (lenshome.c): a stuck sensor costs exactly this. */
#define MAX_SWEEP_STEPS 200

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

/* The attributes the hunt has no business writing: X and Y and the
 * steppers' rail. Each is staged holding SENTINEL. */
#define SENTINEL "untouched\n"
static const char *const not_the_lens[] = {
    "pic/x_step_current", "pic/y_step_current", "cnc/enable", "cnc/disable", "cnc/motor_lock",
};
#define N_NOT_THE_LENS (sizeof(not_the_lens) / sizeof(not_the_lens[0]))

/* A machine whose head is present and whose sensor answers `hall`. */
static void stage(const char *hall)
{
    char dir[256];
    path_of(dir, sizeof(dir), "head");
    mkdir(dir, 0755);
    path_of(dir, sizeof(dir), "cnc");
    mkdir(dir, 0755);
    path_of(dir, sizeof(dir), "pic");
    mkdir(dir, 0755);
    path_of(dir, sizeof(dir), "run");
    mkdir(dir, 0755);
    put("cnc/z_step", "");
    put("head/z_enable", "");
    put("head/z_current", "");
    put("head/z_mode", "");
    for (size_t i = 0; i < N_NOT_THE_LENS; i++)
        put(not_the_lens[i], SENTINEL);
    if (hall)
        put("head/hall_sensor", hall);
    else
        rm("head/hall_sensor");
}

/* 1 when every attribute outside the lens motor holds what stage() put. */
static int not_the_lens_untouched(void)
{
    for (size_t i = 0; i < N_NOT_THE_LENS; i++) {
        char path[256], buf[32] = "";
        path_of(path, sizeof(path), not_the_lens[i]);
        FILE *f = fopen(path, "r");
        if (!f)
            return 0;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (strcmp(buf, SENTINEL) != 0)
            return 0;
    }
    return 1;
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

/* The step attribute as a pipe the test holds both ends of: the sweep's
 * writes never block, and each one is a byte read back by steps_end(). */
static int steps_begin(void)
{
    char path[256];
    path_of(path, sizeof(path), "cnc/z_step");
    unlink(path);
    if (mkfifo(path, 0644) != 0)
        return -1;
    return open(path, O_RDWR | O_NONBLOCK);
}

static int steps_end(int fd)
{
    char buf[4096];
    int n = 0;
    ssize_t r;
    while (fd >= 0 && (r = read(fd, buf, sizeof(buf))) > 0)
        n += (int)r;
    if (fd >= 0)
        close(fd);
    rm("cnc/z_step");
    put("cnc/z_step", "");
    return n;
}

static int read_attr(const char *rel)
{
    char path[256], buf[16] = "";
    path_of(path, sizeof(path), rel);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    return ok ? atoi(buf) : -1;
}

/* A stop input that answers 1 from its nth question on. */
static int stop_at, stop_asked;
static int stop_nth(void)
{
    return ++stop_asked >= stop_at;
}

static void check(const char *what, int cond)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
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
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */

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
    expect("no lens interface", lenshome_run(detail, sizeof(detail), NULL), -1, 0);

    /* No head: its sysfs group never appeared. A fault, because the lens
     * cannot be referenced and a job's focal height would be a guess. */
    stage(NULL);
    expect("no head", lenshome_run(detail, sizeof(detail), NULL), 0, 0);

    /* A dead sensor stuck clear: the carriage never trips it, so the
     * sweep toward it runs out its bound, and not one step past it. An
     * old marker does not outlive the failed hunt. */
    stage("1");
    put("run/lens.home", "edge 1 3 6\n");
    int sfd = steps_begin();
    expect("sensor stuck clear, over an old marker", lenshome_run(detail, sizeof(detail), NULL), 0, 0);
    int steps = steps_end(sfd);
    check("sensor stuck clear: exactly the bound in steps", steps == MAX_SWEEP_STEPS);

    /* A dead sensor stuck tripped, or a carriage that cannot move off it:
     * the sweep away from the sensor runs out its bound. */
    stage("0");
    sfd = steps_begin();
    expect("sensor stuck tripped", lenshome_run(detail, sizeof(detail), NULL), 0, 0);
    steps = steps_end(sfd);
    check("sensor stuck tripped: exactly the bound in steps", steps == MAX_SWEEP_STEPS);

    /* The stop input ends the hunt between two steps: no marker (an old
     * one included), and the motor back at its hold current, half step. */
    stage("0");
    put("run/lens.home", "edge 1 3 6\n");
    stop_at = 3;
    stop_asked = 0;
    sfd = steps_begin();
    expect("a stopped hunt", lenshome_run(detail, sizeof(detail), stop_nth), LENSHOME_STOPPED, 0);
    steps = steps_end(sfd);
    check("a stopped hunt ends at the step its stop came", steps == 2);
    check("a stopped hunt leaves the motor at its hold current and half step",
          read_attr("head/z_current") == 1 && read_attr("head/z_mode") == 1);
    /* ...in the sweep back toward the sensor as in the one away from it:
     * the sensor reads clear, so the first sweep takes no step. */
    stage("1");
    stop_at = 3;
    stop_asked = 0;
    sfd = steps_begin();
    expect("a hunt stopped in its second sweep", lenshome_run(detail, sizeof(detail), stop_nth), LENSHOME_STOPPED, 0);
    steps = steps_end(sfd);
    check("the second sweep ends at the step its stop came", steps == 1);

    /* The healthy sweep, against a carriage the sweep's own steps move. */
    stage("0");
    pid_t child = fork();
    if (child == 0)
        carriage();
    int rc = lenshome_run(detail, sizeof(detail), NULL);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    expect("edge found", rc, 1, 1);
    check("the hunt leaves X, Y, the stepper enable and the motor lock alone",
          not_the_lens_untouched());

    check("the marker reads present", lenshome_present());

    /* The reference is only good until a controller has run. */
    lenshome_clear();
    if (marker_present() || lenshome_present()) {
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
