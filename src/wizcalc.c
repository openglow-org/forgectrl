/*
 * wizcalc.c - the dark wizards' decisions, apart from the hardware
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "wizcalc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(const double *v, int n)
{
    double *c = malloc(sizeof(double) * (size_t)n);
    if (!c)
        return 0.0;
    memcpy(c, v, sizeof(double) * (size_t)n);
    qsort(c, (size_t)n, sizeof(double), cmp_double);
    double m = n % 2 ? c[n / 2] : (c[n / 2 - 1] + c[n / 2]) / 2.0;
    free(c);
    return m;
}

int wizcalc_fan_analyze(const double *rpm, int n, double hz, double min_rpm,
                        wizcalc_fan_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!rpm || hz <= 0.0 || n < (int)(hz * 12.0))
        return -1;
    int tail = (int)(hz * 10.0);
    if (tail < 1)
        tail = 1;
    if (tail > n)
        tail = n;
    out->steady_rpm = median(rpm + (n - tail), tail);
    double target = 0.9 * out->steady_rpm;
    out->spinup_s = (double)n / hz;
    for (int i = 0; i < n; i++) {
        if (rpm[i] >= target) {
            out->spinup_s = (double)i / hz;
            break;
        }
    }
    out->ok = out->steady_rpm >= min_rpm;
    return 0;
}

double wizcalc_floor(double steady, double frac, double lo, double hi)
{
    double f = steady * frac;
    if (f < lo)
        f = lo;
    if (f > hi)
        f = hi;
    return f;
}

double wizcalc_grace_s(double spinup_s, double lo, double hi)
{
    double g = spinup_s + 5.0;
    if (g < lo)
        g = lo;
    if (g > hi)
        g = hi;
    return g;
}

double wizcalc_next_duty(double duty)
{
    if (duty < 40.0)
        return 40.0;
    if (duty < 55.0)
        return 55.0;
    if (duty < 70.0)
        return 70.0;
    return -1.0;
}

static void add_why(char *why, size_t len, int *n, const char *text)
{
    size_t have = strlen(why);
    snprintf(why + have, len - have, "%s%s", have ? ", " : "", text);
    (*n)++;
}

int wizcalc_sensors_judge(const wizcalc_sensors_t *s, char *why, size_t len)
{
    int n = 0;
    char t[96];
    if (len)
        why[0] = '\0';
    if (!isnan(s->down_c) && !isnan(s->up_c)) {
        if (s->down_c < 0.0 || s->down_c > 40.0 || s->up_c < 0.0 || s->up_c > 40.0) {
            snprintf(t, sizeof(t), "coolant %.1f/%.1f C outside 0 to 40", s->down_c, s->up_c);
            add_why(why, len, &n, t);
        } else if (fabs(s->down_c - s->up_c) > 1.5) {
            snprintf(t, sizeof(t), "coolant sensors disagree by %.1f C",
                     fabs(s->down_c - s->up_c));
            add_why(why, len, &n, t);
        }
    } else
        add_why(why, len, &n, "a coolant temperature is unreadable");
    if (!isnan(s->chassis_c) && (s->chassis_c < 0.0 || s->chassis_c > 60.0)) {
        snprintf(t, sizeof(t), "chassis %.1f C outside 0 to 60", s->chassis_c);
        add_why(why, len, &n, t);
    }
    if (!isnan(s->soc_c) && (s->soc_c < 0.0 || s->soc_c > 85.0)) {
        snprintf(t, sizeof(t), "SoC %.1f C outside 0 to 85", s->soc_c);
        add_why(why, len, &n, t);
    }
    for (int i = 0; i < 4; i++) {
        if (s->ir[i] < 0)
            continue;
        if (s->ir_alert > 0.0 && (double)s->ir[i] >= s->ir_alert) {
            snprintf(t, sizeof(t), "lid IR %d reads %ld, at the %.0f alert tier", i + 1,
                     s->ir[i], s->ir_alert);
            add_why(why, len, &n, t);
        }
    }
    if (s->accel_events > 0) {
        snprintf(t, sizeof(t), "%d accelerometer events at rest", s->accel_events);
        add_why(why, len, &n, t);
    }
    if (s->pgood == 0)
        add_why(why, len, &n, "laser supply power-good not sampled good");
    if (s->hv_current > 10) {
        snprintf(t, sizeof(t), "HV current %ld at rest", s->hv_current);
        add_why(why, len, &n, t);
    }
    if (s->tach_exhaust >= 0.0 && s->tach_exhaust < 1.0)
        add_why(why, len, &n, "the exhaust tach reads nothing at idle");
    if (s->tach_intake >= 0.0 && s->tach_intake < 1.0)
        add_why(why, len, &n, "the intake tach reads nothing at idle");
    return n;
}

/* The median, the lower middle of an even count when `lower`, else the
 * upper. */
static long median_of(long *v, int n, int lower)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            long t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    return v[lower ? (n - 1) / 2 : n / 2];
}

/* The ring's two levels before step `at` (1-based, exclusive): the
 * medians of the strong- and quiet-parity steps. Returns the smaller
 * count of the two, 0 when there is nothing to judge. */
static int ring_levels(const long *sums, int at, long *strong_med, long *quiet_med)
{
    long sv[64], qv[64];
    int ns = 0, nq = 0;
    if (at < 3)
        return 0;
    int strong = sums[1] > sums[0] ? 2 : 1;
    for (int i = 1; i < at; i++) {
        if ((i % 2) == (strong % 2)) {
            if (ns < 64)
                sv[ns++] = sums[i - 1];
        } else if (nq < 64) {
            qv[nq++] = sums[i - 1];
        }
    }
    if (ns < 1 || nq < 1)
        return 0;
    *strong_med = median_of(sv, ns, 1);
    *quiet_med = median_of(qv, nq, 0);
    return ns < nq ? ns : nq;
}

int wizcalc_stop_call(const long *sums, int n, int *slipped)
{
    *slipped = 0;
    if (n < 2)
        return 0;
    int strong = sums[1] > sums[0] ? 2 : 1;
    for (int i = 5; i <= n; i++) {
        long s_med, q_med;
        if (ring_levels(sums, i, &s_med, &q_med) < 2)
            continue;
        if (sums[i - 1] * 5 > s_med * 9) {
            *slipped = 1;
            return i;
        }
        if ((i % 2) == (strong % 2) && sums[i - 1] < q_med + (s_med - q_med) / 4)
            return i;
    }
    return 0;
}

int wizcalc_ring_readable(const long *sums, int at, long floor)
{
    long s_med, q_med;
    if (ring_levels(sums, at, &s_med, &q_med) < 2)
        return 0;
    return s_med >= floor && s_med * 2 >= q_med * 3;
}

int wizcalc_z_concur(const long *pos, int n, long tol)
{
    if (n < 2)
        return 0;
    long lo = pos[0], hi = pos[0];
    for (int i = 1; i < n; i++) {
        if (pos[i] < lo)
            lo = pos[i];
        if (pos[i] > hi)
            hi = pos[i];
    }
    return hi - lo <= tol;
}

int wizcalc_jog_witnessed(double rms_x, double rms_y, double rest_x, double rest_y,
                          double ratio, double floor)
{
    double moving = rms_x > rms_y ? rms_x : rms_y;
    double rest = rest_x > rest_y ? rest_x : rest_y;
    return moving >= floor && moving >= ratio * rest;
}

int wizcalc_may_act(const char *owner, const char *requester)
{
    if (!requester || !*requester)
        return 1;
    if (!owner || !*owner)
        return 1;
    return !strcmp(owner, requester);
}
