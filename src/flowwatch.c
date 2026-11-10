/*
 * flowwatch.c - the coolant flow watch (see flowwatch.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "flowwatch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

const char *fw_mode_name(fw_mode_t m)
{
    switch (m) {
    case FW_WAIT: return "wait";
    case FW_REST: return "rest";
    case FW_HOLD: return "hold";
    case FW_SUSPENDED: return "suspended";
    case FW_OFF: return "off";
    }
    return "?";
}

void fw_init(fw_t *w, fw_log_fn log)
{
    memset(w, 0, sizeof(*w));
    w->log = log;
    w->mode = FW_WAIT;
    w->heater_off_at = -1e9;
    w->ref = NAN;
    w->low_ratio = NAN;
    w->last_fans = -1;
    w->fans_changed_at = -1e9;
    w->cfg = flowcalc_cfg_default();
}

static void set_heater(fw_t *w, double pct, double now)
{
    if (w->heater > 0.0 && pct <= 0.0)
        w->heater_off_at = now;
    w->heater = pct > 0.0 ? pct : 0.0;
}

static void rest_begin(fw_t *w, double now, double rest_s)
{
    if (rest_s < FW_REST_MIN_S)
        rest_s = FW_REST_MIN_S;
    w->mode = FW_REST;
    w->rest_until = now + rest_s;
    w->rest_sum = 0.0;
    w->rest_n = 0;
    w->next_rest_sample = now;
    set_heater(w, 0.0, now);
}

static double rest_due(const fw_t *w, double now)
{
    double since = now - w->heater_off_at;
    return since >= FW_REST_AFTER_HEAT ? FW_REST_MIN_S : FW_REST_AFTER_HEAT - since;
}

static void summary_reset(fw_t *w, double now)
{
    w->sum_at = now;
    w->sum_duty = 0.0;
    w->sum_n = 0;
    w->sum_lo = NAN;
    w->sum_hi = NAN;
    w->sum_fall = NAN;
}

static void hold_start(fw_t *w, double now)
{
    w->mode = FW_HOLD;
    w->hold_start = now;
    w->next_pi = now;
    w->high_steps = 0;
    w->seg_ref_noted = 0;
    w->fall_noted = w->session ? w->fall_noted : 0;
    w->fall_now = NAN;
    flowcalc_pi_reset(&w->pi);
    flowcalc_ring_reset(&w->ring);
    summary_reset(w, now);
}

void fw_begin(fw_t *w, double now, double heater_off_ago)
{
    w->heater_off_at = now - heater_off_ago;
    rest_begin(w, now, rest_due(w, now));
}

/* A job's summary, when the hold ran under it at the run fans; a watch
 * that is on but did not hold there says so in one line; a watch that is
 * off says nothing. No number is printed that was never measured. */
static void session_summary(fw_t *w, double now)
{
    double len = now - w->session_at;
    if (!w->run_n) {
        if (w->mode != FW_OFF)
            w->log(LOG_INFO, "flow: job: %.0f s, no hold at the run fans under it (the watch: %s)",
                   len, fw_mode_name(w->mode));
        return;
    }
    char ref_s[96], floor_s[64], fall_s[64];
    if (w->have_ref && !isnan(w->low_ratio)) {
        snprintf(ref_s, sizeof(ref_s), "lowest 10 s %.2f of the idle reference %.2f %%", w->low_ratio, w->ref);
        if (w->floor_noted)
            snprintf(floor_s, sizeof(floor_s), "the floor would have held it at %.0f s", w->floor_at);
        else
            snprintf(floor_s, sizeof(floor_s), "the floor never crossed");
    } else {
        snprintf(ref_s, sizeof(ref_s), "no idle reference");
        snprintf(floor_s, sizeof(floor_s), "the floor judged nothing");
    }
    if (w->fall_at > 0.0)
        snprintf(fall_s, sizeof(fall_s), "the fall detector would have held it at %.0f s", w->fall_at);
    else
        snprintf(fall_s, sizeof(fall_s), "the fall detector never tripped");
    w->log(LOG_INFO, "flow: job: %.0f s, hold duty at the run fans mean %.2f %%, %s, largest fall %.0f %%; %s; %s",
           len, w->run_sum / w->run_n, ref_s, w->worst_fall * 100.0, floor_s, fall_s);
}

static void idle_summary(fw_t *w, double now)
{
    if (w->sum_n && !isnan(w->sum_lo)) {
        char fall_s[48], ref_s[48];
        if (isnan(w->sum_fall))
            snprintf(fall_s, sizeof(fall_s), "no fall judged");
        else
            snprintf(fall_s, sizeof(fall_s), "largest fall %.0f %%", w->sum_fall * 100.0);
        if (w->have_ref)
            snprintf(ref_s, sizeof(ref_s), "idle reference %.2f %%", w->ref);
        else
            snprintf(ref_s, sizeof(ref_s), "no idle reference");
        w->log(LOG_INFO, "flow: idle: %.0f s, hold duty mean %.2f %%, 10 s range %.2f to %.2f %%, %s, %s",
               now - w->sum_at, w->sum_duty / w->sum_n, w->sum_lo, w->sum_hi, fall_s, ref_s);
    }
    summary_reset(w, now);
}

static void hold_step(fw_t *w, const fw_in_t *in)
{
    double now = in->now;
    if (!in->have_means) {
        /* No readings: no duty to hold, so no heat. */
        if (now - w->last_means_at > 2.0 && w->heater > 0.0) {
            set_heater(w, 0.0, now);
            flowcalc_pi_reset(&w->pi);
            if (!w->means_lost_noted) {
                w->means_lost_noted = 1;
                w->log(LOG_WARNING, "flow: the sampler gives no readings; the heater off until it does");
            }
        }
        return;
    }
    w->means_lost_noted = 0;
    if (now < w->next_pi)
        return;
    w->next_pi += FLOWCALC_DUTY_DT;
    if (w->next_pi < now)
        w->next_pi = now + FLOWCALC_DUTY_DT;
    double duty = flowcalc_pi_step(&w->pi, &w->cfg, (in->down_c - in->up_c) - w->rest_c, FLOWCALC_DUTY_DT);
    if (in->down_c >= FW_RAIL_C) {
        duty = 0.0;
        flowcalc_pi_reset(&w->pi);
    }
    w->duty = duty;
    set_heater(w, duty, now);
    flowcalc_ring_push(&w->ring, duty);
    if (duty >= w->cfg.duty_max) {
        if (++w->high_steps >= (int)(FW_HIGH_S / FLOWCALC_DUTY_DT)) {
            set_heater(w, 0.0, now);
            w->mode = FW_OFF;
            w->halted = 1;
            w->log(LOG_WARNING, "flow: the hold sat at its most (%.1f %%) for %.0f s: a frozen or dead reading, or "
                   "no heater; the watch stops until the next start", w->cfg.duty_max, FW_HIGH_S);
            return;
        }
    } else {
        w->high_steps = 0;
    }
    double m10 = flowcalc_ring_mean(&w->ring, 10.0, 0.0);
    int idle = !w->session && in->fans == FW_FANS_IDLE;
    /* Seconds at these fans in this hold. */
    double since = now - (w->fans_changed_at > w->hold_start ? w->fans_changed_at : w->hold_start);
    /* The idle reference, and each later idle stretch's settled duty, from
     * its own 60 to 90 s at the idle fans: a job that starts before the
     * first one is taken leaves the reference to the idle after it. */
    if (idle && !w->seg_ref_noted && since >= FW_REF_TO_S) {
        double m = flowcalc_ring_mean(&w->ring, FW_REF_TO_S - FW_REF_FROM_S, 0.0);
        if (!isnan(m)) {
            w->seg_ref_noted = 1;
            if (!w->have_ref && m <= 0.0) {
                /* A hold that settles at no duty is nothing to measure a
                 * job against: no reference, and the floor judges nothing
                 * until an idle stretch gives one. */
                w->log(LOG_NOTICE, "flow: no idle reference: the hold settled at no duty at the idle fans "
                       "(rest %+.3f C, up %.2f C)", w->rest_c, in->up_c);
            } else if (!w->have_ref) {
                w->ref = m;
                w->have_ref = 1;
                w->log(LOG_INFO, "flow: the idle reference is %.2f %% (rest %+.3f C, up %.2f C)",
                       m, w->rest_c, in->up_c);
            } else {
                w->log(LOG_INFO, "flow: idle duty %.2f %%, %.2f of the reference", m, m / w->ref);
            }
        }
    }
    double fall = since >= FW_BLIND_S ? flowcalc_fall(&w->ring) : NAN;
    w->fall_now = fall;
    if (idle) {
        w->sum_duty += duty;
        w->sum_n++;
        if (!isnan(m10)) {
            if (isnan(w->sum_lo) || m10 < w->sum_lo)
                w->sum_lo = m10;
            if (isnan(w->sum_hi) || m10 > w->sum_hi)
                w->sum_hi = m10;
        }
        if (!isnan(fall) && (isnan(w->sum_fall) || fall > w->sum_fall))
            w->sum_fall = fall;
        if (now - w->sum_at >= FW_SUMMARY_S)
            idle_summary(w, now);
    }
    if (w->session && in->fans == FW_FANS_RUN) {
        w->run_sum += duty;
        w->run_n++;
        if (w->have_ref && !isnan(m10) && since >= FW_FLOOR_SETTLE_S) {
            double ratio = m10 / w->ref;
            if (isnan(w->low_ratio) || ratio < w->low_ratio)
                w->low_ratio = ratio;
            if (m10 < w->ref && !w->floor_noted) {
                w->floor_noted = 1;
                w->floor_at = now - w->session_at;
                w->log(LOG_NOTICE, "flow: floor: the 10 s duty %.2f %% is under the idle reference %.2f %% at the "
                       "run fans, %.0f s into the job: the floor would hold it", m10, w->ref, w->floor_at);
            }
        }
    }
    if (!isnan(fall)) {
        if (w->session && fall > w->worst_fall)
            w->worst_fall = fall;
        if (fall >= FLOWCALC_FALL_LINE && !w->fall_noted) {
            w->fall_noted = 1;
            if (w->session)
                w->fall_at = now - w->session_at;
            w->log(LOG_NOTICE, "flow: fall: the 10 s duty %.2f %% fell %.0f %% against 60 to 90 s before, %s: "
                   "the fall detector would hold %s", m10, fall * 100.0,
                   w->session ? "under a job" : "at idle", w->session ? "the job" : "the next job");
        }
    }
}

double fw_step(fw_t *w, const fw_in_t *in)
{
    double now = in->now;
    if (w->mode == FW_WAIT)
        return 0.0;
    if (in->have_means)
        w->last_means_at = now;

    if (in->fans != w->last_fans) {
        if (w->last_fans >= 0) {
            w->fans_changed_at = now;
            w->seg_ref_noted = 0;
        }
        w->last_fans = in->fans;
    }
    if (in->session != w->session) {
        if (in->session) {
            w->session = 1;
            w->session_at = now;
            w->floor_noted = 0;
            w->fall_noted = 0;
            w->floor_at = w->fall_at = 0.0;
            w->worst_fall = 0.0;
            w->low_ratio = NAN;
            w->run_sum = 0.0;
            w->run_n = 0;
        } else {
            session_summary(w, now);
            w->session = 0;
            w->fall_noted = 0;
            summary_reset(w, now);
        }
    }

    /* The setting, and the engine taking the heater. A comeback rests the
     * full time: the engine may have run the heater meanwhile. */
    if (!in->enabled) {
        if (w->mode != FW_OFF) {
            w->log(LOG_NOTICE, "flow: the watch is off (cool_flow_watch is 0)");
            w->mode = FW_OFF;
        }
    } else if (in->yield) {
        if (w->mode != FW_SUSPENDED && w->mode != FW_OFF) {
            w->yield_why = in->yield_why ? in->yield_why : "the engine needs it";
            w->log(LOG_INFO, "flow: the watch gives the heater back: %s", w->yield_why);
            w->mode = FW_SUSPENDED;
        }
    } else if (w->mode == FW_SUSPENDED || (w->mode == FW_OFF && !w->halted)) {
        w->log(LOG_INFO, "flow: the watch has the heater again; the hold after a rest");
        rest_begin(w, now, FW_REST_AFTER_HEAT);
    }

    /* The owner's numbers, held to their ranges; a change starts the hold
     * over, its idle reference with it (the duty means something only
     * against itself at the same step and limits). */
    flowcalc_cfg_t cfg = flowcalc_cfg_clamp(in->cfg);
    if (cfg.hold_c != w->cfg.hold_c || cfg.duty_min != w->cfg.duty_min || cfg.duty_max != w->cfg.duty_max) {
        w->cfg = cfg;
        w->have_ref = 0;
        w->ref = NAN;
        if (w->mode == FW_HOLD) {
            w->log(LOG_INFO, "flow: the hold's settings changed (step %.2f C, duty %.1f to %.1f %%): the hold starts "
                   "over", cfg.hold_c, cfg.duty_min, cfg.duty_max);
            hold_start(w, now);
        }
    }

    switch (w->mode) {
    case FW_WAIT:
    case FW_OFF:
    case FW_SUSPENDED:
        set_heater(w, 0.0, now);
        break;
    case FW_REST:
        set_heater(w, 0.0, now);
        if (in->have_means && now >= w->next_rest_sample && now >= w->rest_until - FW_REST_MEAN_S) {
            w->next_rest_sample = now + FLOWCALC_DUTY_DT;
            w->rest_sum += in->down_c - in->up_c;
            w->rest_n++;
        }
        if (now >= w->rest_until) {
            if (!w->rest_n) {
                /* No readings over the rest: rest again, and say so once
                 * (a kernel module with no sampler never gives any). */
                if (!w->means_lost_noted) {
                    w->means_lost_noted = 1;
                    w->log(LOG_WARNING, "flow: the sampler gives no readings (pic/means); the hold waits for them");
                }
                rest_begin(w, now, FW_REST_MIN_S);
                break;
            }
            w->rest_c = w->rest_sum / w->rest_n;
            w->have_rest = 1;
            hold_start(w, now);
            w->log(LOG_INFO, "flow: the hold starts (step %.2f C, duty %.1f to %.1f %%, rest %+.3f C, up %.2f C, "
                   "down %.2f C; log only)", w->cfg.hold_c, w->cfg.duty_min, w->cfg.duty_max,
                   w->rest_c, in->have_means ? in->up_c : NAN, in->have_means ? in->down_c : NAN);
        }
        break;
    case FW_HOLD:
        hold_step(w, in);
        break;
    }
    return w->heater;
}

static void jnum(char *buf, size_t len, size_t *n, const char *key, double v, int dp)
{
    if (*n >= len)
        return;
    if (isnan(v) || isinf(v))
        *n += (size_t)snprintf(buf + *n, len - *n, ",\"%s\":null", key);
    else
        *n += (size_t)snprintf(buf + *n, len - *n, ",\"%s\":%.*f", key, dp, v);
}

int fw_status_json(const fw_t *w, char *buf, size_t len)
{
    /* why: what keeps it from holding, for the panel ("" while it holds
     * or rests, or when the setting has it off) */
    const char *why = "";
    if (w->halted)
        why = "the hold sat at its most: a dead reading or heater; stopped until the next start";
    else if (w->mode == FW_SUSPENDED && w->yield_why)
        why = w->yield_why;
    else if ((w->mode == FW_HOLD || w->mode == FW_REST) && w->means_lost_noted)
        why = "the sampler gives no readings";
    size_t n = (size_t)snprintf(buf, len, "{\"watch\":\"%s\",\"halted\":%s,\"why\":\"%s\"", fw_mode_name(w->mode),
                                w->halted ? "true" : "false", why);
    jnum(buf, len, &n, "duty", w->mode == FW_HOLD ? w->heater : NAN, 2);     /* commanded */
    jnum(buf, len, &n, "duty_10s", w->mode == FW_HOLD ? flowcalc_ring_mean(&w->ring, 10.0, 0.0) : NAN, 2);
    jnum(buf, len, &n, "ref", w->ref, 2);
    jnum(buf, len, &n, "fall", w->mode == FW_HOLD ? w->fall_now : NAN, 3);     /* null while blind */
    jnum(buf, len, &n, "rest_c", w->have_rest ? w->rest_c : NAN, 3);
    jnum(buf, len, &n, "hold_c", w->cfg.hold_c, 2);
    jnum(buf, len, &n, "duty_min", w->cfg.duty_min, 1);
    jnum(buf, len, &n, "duty_max", w->cfg.duty_max, 1);
    if (n < len)
        n += (size_t)snprintf(buf + n, len - n, "}");
    return n < len ? (int)n : -1;
}
