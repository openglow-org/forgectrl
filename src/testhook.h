/*
 * testhook.h - the host tests' seams, behind one gate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * FORGECTRL_HOST_TEST=1 says the daemon runs on a host under a host
 * test: main.c then leaves the radio alone, super.c the OOM scores, and
 * the test hooks are honored. A test hook moves a machine path for a
 * host test: GF_SYSFS_ROOT the sysfs tree of the kernel module (it ends
 * with '/'), GF_SWITCH_DEV the switch device, GF_RUN_DIR the run
 * directory, GF_DEV_MARKER the dev-image marker (/etc/forgefirm-dev),
 * GF_DATA_STATE the boot's data record (/run/forgefirm-data.state),
 * GF_LED_ROOT the LED class (/sys/class/leds), GF_IIO_ROOT the iio
 * devices the motion probe finds the head accelerometer among
 * (/sys/bus/iio/devices), GF_PULSE_DEV the pulse device (/dev/glowforge),
 * GF_SYSFS_DIR the lens hunt's sysfs tree (/sys/glowforge, without the
 * trailing '/'), GF_GRBL_PORT the Grbl port the daemon's streamers dial
 * (23), GF_PROC_STAT the CPU counters (/proc/stat). Two change what the
 * daemon reads or how it moves: GF_SWITCH_WORD_FILE names a file holding
 * the switch word in place of the switch device's, and GF_LENS_STEP_US
 * shortens the lens hunt's step cadence. Two more stand in for a
 * program a host test must not run: GF_SLOT_LIST names a file that holds
 * the slot inventory in place of `ffboot -l`, and GF_REBOOT_FILE a file
 * a reboot the daemon schedules is written to in place of the reboot
 * (the reboot stands pending while that file does: the test removes it
 * for the restart), and GF_USERS_REPLAY a script run in place of the account replay
 * (/etc/init.d/forgefirm-users). GF_SLOT_DIR moves the slot table into a
 * scratch directory (each slot's device a file there, the booted root the
 * file `root` links to, each size under sys/), so an update job writes
 * and verifies a file and never a block device; GF_VERSION_FILE names the
 * installed version's file (/etc/forgefirm-version); GF_RELEASE_HTTP lets
 * the release check and the download take a plain-HTTP release server
 * (curl takes HTTPS alone otherwise). GF_HTTP_IDLE_S shortens the listeners'
 * idle connection timeout, GF_PRESS_TIMEOUT_S the Setup page's press
 * wait and its request's life, GF_PRESS_RES_S the renewal its poll gives
 * the press's reservation, GF_RESET_HOLD_S the button hold at start
 * that resets the account, and GF_PRESS_DEBOUNCE_MS lengthens how long a
 * press must still be held after the wait first reads it. GF_PAUSE_DIR
 * names a directory of pauses (ff_test_pause): a route that reaches a
 * named point waits there while the file of that name stands in it, so a
 * test lands a second request between a route's check and its act.
 * GF_RUN_END_GAP_MS pauses an update job and a setup check that long
 * once each reads ended, so a host test can start the next in that gap.
 * ff_test_hook() returns the hook's value in a host-test run
 * and NULL in any other, so on a machine every reader takes its machine
 * path whatever the environment says: the switch device is the motion
 * gate's and the privacy gate's input, and a moved device would be a
 * lid that reads closed when it is not. The sources read a test hook
 * only through this gate; main.c's TEST_HOOKS names the hooks, the
 * start notice names the ones in force, and the ones set and ignored,
 * and tests/test_routes.py holds the list to the sources.
 */
#ifndef FORGECTRL_TESTHOOK_H
#define FORGECTRL_TESTHOOK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline int ff_host_test(void)
{
    const char *v = getenv("FORGECTRL_HOST_TEST");
    return v && strcmp(v, "1") == 0;
}

/* The hook's value, when it is set and not empty, in a host-test run;
 * NULL otherwise. */
static inline const char *ff_test_hook(const char *hook)
{
    if (!ff_host_test())
        return NULL;
    const char *v = getenv(hook);
    return v && *v ? v : NULL;
}

/* A host test's pause at a named point: while <GF_PAUSE_DIR>/<point>
 * stands, the caller waits here (30 s at most), with <point>.at beside it
 * saying it has arrived. Nothing in any other run, or with no such file. */
static inline void ff_test_pause(const char *point)
{
    const char *dir = ff_test_hook("GF_PAUSE_DIR");
    char path[512], at[520];
    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/%s", dir, point);
    if (access(path, F_OK) != 0)
        return;
    snprintf(at, sizeof(at), "%s.at", path);
    FILE *f = fopen(at, "w");
    if (f)
        fclose(f);
    struct timespec tick = {0, 20 * 1000 * 1000};
    for (int i = 0; i < 1500 && access(path, F_OK) == 0; i++)
        nanosleep(&tick, NULL);
    unlink(at);
}
#endif
