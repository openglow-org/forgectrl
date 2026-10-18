/*
 * holds_wire_test.c - host test: the extension host's real hold files, read by the engine's real reader
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * forgeext's holdkeep.c writes into a directory; forgectrl's holds.c reads
 * it. What the two daemons must agree on is proven here together, built
 * from the host's own source: the holds directory literal, the table
 * sizes, the write period against the reader's freshness window, the
 * files the host writes read as the holds it meant (their names, their
 * fields, the host's own words), and a silent host's files going stale.
 * The required-holds markers are the second agreement: the directory
 * literal, install.h's against holds.h's, and a marker written as
 * install.c writes it, one empty file named after the package, which
 * holds alone when no hold file speaks for the package and is answered
 * by the host's file of the same name. The test reads the files
 * on CLOCK_MONOTONIC, and the target wraps clock_gettime: every other
 * clock reads 2000000000 s, as in cool_flow_test's fake clock. A host that
 * stamps ts_mono on another clock writes files that read stale, and the
 * test fails. cool_flow_test holds the engine to CLOCK_MONOTONIC.
 */
#define _GNU_SOURCE
#include "../src/holds.h"
#include "holdkeep.h"
#include "install.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* clock_gettime as this file, holds.c and holdkeep.c call it (the target
 * links with --wrap=clock_gettime): CLOCK_MONOTONIC passes through, and
 * every other clock reads 2000000000 s, so a stamp from another clock is
 * far from the reader's time. */
int __real_clock_gettime(clockid_t clk, struct timespec *ts);

int __wrap_clock_gettime(clockid_t clk, struct timespec *ts)
{
    if (clk != CLOCK_MONOTONIC) {
        ts->tv_sec = 2000000000;
        ts->tv_nsec = 0;
        return 0;
    }
    return __real_clock_gettime(clk, ts);
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap(double s)
{
    struct timespec t = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&t, NULL);
}

int main(void)
{
    char dir[] = "/tmp/holds-wire-XXXXXX", req[] = "/tmp/holds-wire-req-XXXXXX", err[200];
    hold_entry_t e[2];
    holds_t h;
    static holdkeep_t k;

    /* The holds directory literal: one home on each side. */
    CHECK(!strcmp(HOLDKEEP_DIR_DEFAULT, HOLDS_DIR_DEFAULT), "the holds directory differs: host %s, engine %s",
          HOLDKEEP_DIR_DEFAULT, HOLDS_DIR_DEFAULT);
    /* The required-holds marker directory literal: the host writes it (install.c), the engine reads it. */
    CHECK(!strcmp(EXT_REQUIRED_HOLDS_DIR, HOLDS_REQUIRED_DIR), "the required-holds directory differs: host %s, engine %s",
          EXT_REQUIRED_HOLDS_DIR, HOLDS_REQUIRED_DIR);
    /* The table sizes: the host never writes more files than the engine reads. */
    CHECK(HOLDKEEP_MAX <= HOLDS_MAX, "the host keeps %d holds, the engine reads %d", HOLDKEEP_MAX, HOLDS_MAX);
    /* The write period against the reader's window: a missed write still reads fresh. */
    CHECK(2.0 * HOLDKEEP_PERIOD_MS / 1000.0 < HOLDS_FRESH_S, "two write periods (%d ms) do not fit the window (%.1f s)",
          HOLDKEEP_PERIOD_MS, HOLDS_FRESH_S);

    if (!mkdtemp(dir) || !mkdtemp(req)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }

    /* The host writes: a required hold clear, an advisory one raised. */
    CHECK(holdkeep_start(&k, dir, HOLDKEEP_MAIN_S, err, sizeof(err)) == 0, "the keeper did not start: %s", err);
    hold_decide("org.example.badge", 1, 1, 1, 0, NULL, &e[0]);
    hold_decide("org.example.filter", 0, 1, 0, 1, "filter life under 5 percent", &e[1]);
    holdkeep_set(&k, e, 2);
    holds_read(dir, req, mono(), &h);
    CHECK(h.files == 2 && h.unreadable == 0, "the engine did not read the host's two files as holds: files %d, unreadable %d (%s)",
          h.files, h.unreadable, h.reason);
    CHECK(h.standing && h.raised == 1 && h.stale_required == 0 && h.stale_advisory == 0 &&
          !strcmp(h.reason, "org.example.filter: filter life under 5 percent"),
          "the advisory hold the package raised does not stand in its words: standing %d raised %d stale %d/%d reason %s",
          h.standing, h.raised, h.stale_required, h.stale_advisory, h.reason);

    /* The package raises the required one in its own words: first by name. */
    hold_decide("org.example.badge", 1, 1, 1, 1, "no badge presented", &e[0]);
    holdkeep_set(&k, e, 2);
    holds_read(dir, req, mono(), &h);
    CHECK(h.standing && h.raised == 2 && !strcmp(h.reason, "org.example.badge: no badge presented"),
          "the raised required hold does not stand in the package's words: raised %d reason %s", h.raised, h.reason);

    /* The host's own words for a package that cannot speak. */
    hold_decide("org.example.badge", 1, 0, 0, 0, NULL, &e[0]);
    holdkeep_set(&k, e, 2);
    holds_read(dir, req, mono(), &h);
    CHECK(h.standing && !strcmp(h.reason, "org.example.badge: the extension is not running"),
          "the host's words for a package that is not running do not reach the verdict: %s", h.reason);

    /* Both clear: nothing stands. */
    hold_decide("org.example.badge", 1, 1, 1, 0, NULL, &e[0]);
    hold_decide("org.example.filter", 0, 1, 0, 0, NULL, &e[1]);
    holdkeep_set(&k, e, 2);
    holds_read(dir, req, mono(), &h);
    CHECK(!h.standing && h.files == 2, "two clear holds stand: %s", h.reason);

    /* The keeper's thread, not the main loop, keeps them fresh: no set for longer than the
       window, and the engine still reads them fresh. */
    nap(HOLDS_FRESH_S + 0.6);
    holds_read(dir, req, mono(), &h);
    CHECK(!h.standing && h.stale_required == 0 && h.stale_advisory == 0,
          "the thread's rewrites did not keep the files fresh across %.1f s: stale %d/%d (%s)",
          HOLDS_FRESH_S + 0.6, h.stale_required, h.stale_advisory, h.reason);

    /* A host whose main loop has gone silent: the thread stops writing, the files go stale on
       the engine's clock, the required hold stands in the engine's words, the advisory is
       dropped. The silence bound is shortened for the test; the window is the reader's own. */
    holdkeep_stop(&k);
    CHECK(holdkeep_start(&k, dir, 0.3, err, sizeof(err)) == 0, "the keeper did not restart: %s", err);
    hold_decide("org.example.badge", 1, 1, 1, 0, NULL, &e[0]);
    hold_decide("org.example.filter", 0, 1, 0, 1, "filter life under 5 percent", &e[1]);
    holdkeep_set(&k, e, 2);
    nap(HOLDS_FRESH_S + 0.6);
    holds_read(dir, req, mono(), &h);
    CHECK(h.standing && h.stale_required == 1 && h.stale_advisory == 1 && h.raised == 0 &&
          !strcmp(h.reason, "org.example.badge: the extension host is not answering"),
          "a silent host: standing %d, stale required %d, stale advisory %d, raised %d, reason %s",
          h.standing, h.stale_required, h.stale_advisory, h.raised, h.reason);

    /* A clean stop: the advisory file goes, and the required one is left to stand stale. */
    holdkeep_set(&k, e, 2);
    holdkeep_stop(&k);
    holds_read(dir, req, mono(), &h);
    CHECK(h.files == 1 && h.raised == 0, "after a clean stop: %d files, raised %d (%s)", h.files, h.raised, h.reason);
    nap(HOLDS_FRESH_S + 0.6);
    holds_read(dir, req, mono(), &h);
    CHECK(h.standing && h.stale_required == 1 && !strcmp(h.reason, "org.example.badge: the extension host is not answering"),
          "the required file a stopped host left does not stand stale: %d %s", h.standing, h.reason);

    /* The marker alone: the hold files are gone (a reboot takes the tmpfs) and the host has not
       written any, and the operator marked the package's hold required. The marker is written as
       install.c's ext_required_holds_sync writes it, one empty file named after the package, and
       the hold stands in the engine's words with no file read. */
    char path[400];
    snprintf(path, sizeof(path), "%s/org.example.badge.json", dir);
    CHECK(unlink(path) == 0, "the required file is not where the host wrote it: %s", path);
    snprintf(path, sizeof(path), "%s/org.example.badge", req);
    int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    CHECK(fd >= 0, "the marker could not be written: %s", path);
    if (fd >= 0)
        close(fd);
    holds_read(dir, req, mono(), &h);
    CHECK(h.standing && h.files == 0 && h.stale_required == 1 &&
          !strcmp(h.reason, "org.example.badge: the extension host is not answering"),
          "a marker with no hold file does not hold: standing %d, files %d, stale required %d, reason %s",
          h.standing, h.files, h.stale_required, h.reason);

    /* The host comes up and speaks for the package: its file answers the marker by the same name,
       and nothing stands. */
    CHECK(holdkeep_start(&k, dir, HOLDKEEP_MAIN_S, err, sizeof(err)) == 0, "the keeper did not start again: %s", err);
    hold_decide("org.example.badge", 1, 1, 1, 0, NULL, &e[0]);
    holdkeep_set(&k, e, 1);
    holds_read(dir, req, mono(), &h);
    CHECK(!h.standing && h.files == 1 && h.stale_required == 0,
          "the host's file does not answer the marker of the same name: standing %d, files %d, stale required %d (%s)",
          h.standing, h.files, h.stale_required, h.reason);
    holdkeep_stop(&k);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", dir, req);
    if (system(cmd) != 0)
        fails++;
    printf("%s: holds_wire_test, %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
