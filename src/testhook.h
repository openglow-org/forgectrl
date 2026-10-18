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
 * directory. ff_test_hook() returns the hook's value in a host-test run
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
#include <stdlib.h>
#include <string.h>

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
#endif
