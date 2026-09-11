/*
 * lens.c - the lens frame (see lens.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "lens.h"
#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

double lens_edge_z(void)
{
    char v[32];
    if (settings_get("lens_hall_edge_z_mm", v, sizeof(v)) == 0 && v[0]) {
        char *end;
        double z = strtod(v, &end);
        if (end != v)
            return z;
    }
    return LENS_EDGE_Z_DEFAULT;
}

static int stop_count(const char *key, int fallback, int *from_card)
{
    char v[32];
    if (settings_get(key, v, sizeof(v)) == 0 && v[0]) {
        char *end;
        double n = strtod(v, &end);
        if (end != v && n >= 1 && n <= LENS_STOP_STEPS_MAX) {
            *from_card = 1;
            return (int)n;
        }
    }
    *from_card = 0;
    return fallback;
}

int lens_window(int *down, int *up)
{
    int a, b;
    *down = stop_count("lens_stop_below_steps", LENS_WINDOW_DOWN, &a);
    *up = stop_count("lens_stop_above_steps", LENS_WINDOW_UP, &b);
    return a && b;
}

double lens_z_of_k(double edge_z, double spm, double k)
{
    return edge_z + k / spm;
}

void lens_reach(double edge_z, double spm, int down, int up, double *lo, double *hi)
{
    *lo = lens_z_of_k(edge_z, spm, -down);
    *hi = lens_z_of_k(edge_z, spm, up);
}

void lens_envelope(double edge_z, double spm, int down, int up, double *lo, double *hi)
{
    *lo = lens_z_of_k(edge_z, spm, -(down + 1));
    *hi = lens_z_of_k(edge_z, spm, up + 1);
}

void lens_ladder_k(int down, int up, int n, int *k)
{
    for (int i = 0; i < n; i++)
        k[i] = n < 2 ? up : (int)lround(up - (double)(up + down) * i / (n - 1));
}

int lens_line_z(const char *line, double *z)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, "G0 ", 3) != 0 && strncmp(line, "G1 ", 3) != 0 &&
        strncmp(line, "G00 ", 4) != 0 && strncmp(line, "G01 ", 4) != 0)
        return 0;
    for (const char *p = line + 2; *p && *p != ';' && *p != '\n' && *p != '\r'; p++) {
        if (*p == 'Z' && p[-1] == ' ') {
            char *end;
            double v = strtod(p + 1, &end);
            if (end == p + 1)
                return 0;
            *z = v;
            return 1;
        }
    }
    return 0;
}
