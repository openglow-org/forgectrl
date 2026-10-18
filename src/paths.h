/*
 * paths.h - the daemon's persistent and runtime directories
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * /data/forgefirm holds ForgeFIRM's own persistent files (the panel
 * token, the account record, the setup record, the sheet salt,
 * the TLS key and certificate, the GRBL controller's settings store);
 * /run/forgefirm holds the runtime markers and the login sessions. Host
 * tests point both at a scratch tree through the environment: the data
 * directory through FORGECTRL_DATA_DIR, the run directory through the
 * test hook GF_RUN_DIR, honored in a host-test run alone (testhook.h).
 */
#ifndef FORGECTRL_PATHS_H
#define FORGECTRL_PATHS_H
#include <stdlib.h>
#include "testhook.h"

static inline const char *ff_data_dir(void)
{
    const char *d = getenv("FORGECTRL_DATA_DIR");
    return d && *d ? d : "/data/forgefirm";
}

static inline const char *ff_run_dir(void)
{
    const char *d = ff_test_hook("GF_RUN_DIR");
    return d ? d : "/run/forgefirm";
}
#endif
