/*
 * paths.h - the daemon's persistent and runtime directories
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * /data/forgefirm holds ForgeFIRM's own persistent files (the panel
 * token, the account record, the commissioning record, the sheet salt,
 * the TLS key and certificate, the GRBL controller's settings store);
 * /run/forgefirm holds the runtime markers and the login sessions. Host
 * tests point both at a scratch tree through the environment.
 */
#ifndef FORGECTRL_PATHS_H
#define FORGECTRL_PATHS_H
#include <stdlib.h>

static inline const char *ff_data_dir(void)
{
    const char *d = getenv("FORGECTRL_DATA_DIR");
    return d && *d ? d : "/data/forgefirm";
}

static inline const char *ff_run_dir(void)
{
    const char *d = getenv("GF_RUN_DIR");
    return d && *d ? d : "/run/forgefirm";
}
#endif
