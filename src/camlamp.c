/*
 * camlamp.c - a camera lamp write and its result
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "camlamp.h"

#include <errno.h>
#include <stdio.h>

int camlamp_write(const char *path, int level)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return errno ? errno : EIO;
    int e = fprintf(f, "%d", level) > 0 ? 0 : (errno ? errno : EIO);
    if (fclose(f) && !e)
        e = errno ? errno : EIO;
    return e;
}
