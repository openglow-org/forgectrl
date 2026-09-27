/*
 * tray.c - the crumb tray, in or out (see tray.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "tray.h"
#include "lens.h"
#include "paths.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int tray_is_out(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/tray.out", ff_data_dir());
    return access(path, F_OK) == 0;
}

static double clamp_offset(double mm)
{
    return mm < TRAY_OFFSET_MIN_MM ? TRAY_OFFSET_MIN_MM
         : mm > TRAY_OFFSET_MAX_MM ? TRAY_OFFSET_MAX_MM : mm;
}

double tray_offset_mm(void)
{
    char v[32];
    if (settings_get("tray_offset_mm", v, sizeof(v)) == 0 && v[0]) {
        char *end;
        double mm = strtod(v, &end);
        if (end != v && mm == mm)
            return clamp_offset(mm);
    }
    return TRAY_OFFSET_DEFAULT_MM;
}

double tray_grid_mm(void)
{
    /* The controller's arithmetic, in float, so the two land on the same
     * half-step. */
    float spm = (float)LENS_STEPS_PER_MM;
    return (double)lroundf((float)tray_offset_mm() * spm) / spm;
}

double tray_shift_mm(void)
{
    return tray_is_out() ? tray_grid_mm() : 0.0;
}

int tray_offset_valid(const char *v)
{
    char *end;
    double mm = strtod(v, &end);
    return end != v && *end == '\0' && mm >= TRAY_OFFSET_MIN_MM && mm <= TRAY_OFFSET_MAX_MM;
}

int tray_offset_refusal(const char *v, char *why, size_t len)
{
    char cur[32];
    if (!v || !tray_is_out())
        return 0;
    if (settings_get("tray_offset_mm", cur, sizeof(cur)) != 0)
        cur[0] = '\0';
    if (!strcmp(cur, v))
        return 0;           /* no change */
    snprintf(why, len, "set the tray in first: the tray offset changes only while the tray is in");
    return 1;
}

int tray_setup_refusal(char *why, size_t len)
{
    if (!tray_is_out())
        return 0;
    snprintf(why, len, "Put the crumb tray in and set Tray in on the Machine tab.");
    return 1;
}
