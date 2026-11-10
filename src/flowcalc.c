/*
 * flowcalc.c - the coolant flow watch's arithmetic (see flowcalc.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "flowcalc.h"

#include <math.h>

flowcalc_cfg_t flowcalc_cfg_default(void)
{
    flowcalc_cfg_t c = { FLOWCALC_HOLD_C_DEF, FLOWCALC_DUTY_MIN_DEF, FLOWCALC_DUTY_MAX_DEF };
    return c;
}

static double held(double v, double lo, double hi, double def)
{
    if (isnan(v) || isinf(v))
        return def;
    return v < lo ? lo : v > hi ? hi : v;
}

flowcalc_cfg_t flowcalc_cfg_clamp(flowcalc_cfg_t c)
{
    c.hold_c = held(c.hold_c, FLOWCALC_HOLD_C_LO, FLOWCALC_HOLD_C_HI, FLOWCALC_HOLD_C_DEF);
    c.duty_min = held(c.duty_min, FLOWCALC_DUTY_MIN_LO, FLOWCALC_DUTY_MIN_HI, FLOWCALC_DUTY_MIN_DEF);
    c.duty_max = held(c.duty_max, FLOWCALC_DUTY_MAX_LO, FLOWCALC_DUTY_MAX_HI, FLOWCALC_DUTY_MAX_DEF);
    return c;
}

void flowcalc_pi_reset(flowcalc_pi_t *pi)
{
    pi->integ = 0.0;
}

double flowcalc_pi_step(flowcalc_pi_t *pi, const flowcalc_cfg_t *c, double pv, double dt)
{
    double err = c->hold_c - pv;
    double integ = pi->integ + FLOWCALC_KI * err * dt;
    double out = FLOWCALC_KP * err + integ;
    /* The integral moves only while the output is inside the rails, or
     * when the step would bring it back toward them. */
    if (out > c->duty_max) {
        out = c->duty_max;
        if (err < 0.0)
            pi->integ = integ;
    } else if (out < c->duty_min) {
        out = c->duty_min;
        if (err > 0.0)
            pi->integ = integ;
    } else {
        pi->integ = integ;
    }
    if (pi->integ < 0.0)
        pi->integ = 0.0;
    if (pi->integ > c->duty_max)
        pi->integ = c->duty_max;
    return out;
}

void flowcalc_ring_reset(flowcalc_ring_t *r)
{
    r->head = 0;
    r->count = 0;
}

void flowcalc_ring_push(flowcalc_ring_t *r, double v)
{
    r->v[r->head] = v;
    r->head = (r->head + 1) % FLOWCALC_DUTY_N;
    if (r->count < FLOWCALC_DUTY_N)
        r->count++;
}

double flowcalc_ring_mean(const flowcalc_ring_t *r, double from_s, double to_s)
{
    size_t newest_skip = (size_t)(to_s / FLOWCALC_DUTY_DT + 0.5);
    size_t span = (size_t)((from_s - to_s) / FLOWCALC_DUTY_DT + 0.5);
    if (span == 0 || newest_skip + span > r->count)
        return NAN;
    double sum = 0.0;
    for (size_t i = 0; i < span; i++) {
        size_t back = newest_skip + i + 1;          /* 1 = the newest value */
        size_t idx = (r->head + FLOWCALC_DUTY_N - back) % FLOWCALC_DUTY_N;
        sum += r->v[idx];
    }
    return sum / (double)span;
}

double flowcalc_fall(const flowcalc_ring_t *r)
{
    double now = flowcalc_ring_mean(r, 10.0, 0.0);
    double ref = flowcalc_ring_mean(r, 90.0, 60.0);
    if (isnan(now) || isnan(ref) || ref <= 0.0)
        return NAN;
    return 1.0 - now / ref;
}
