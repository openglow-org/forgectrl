/*
 * senslogfmt.c - the sensors logger's records (see senslogfmt.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "senslogfmt.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------ output */

typedef struct {
    char *b;
    size_t len, off;
    int over;
} out_t;

static void put(out_t *o, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (o->over)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->b + o->off, o->len - o->off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->len - o->off) {
        o->over = 1;
        return;
    }
    o->off += (size_t)n;
}

static void put_l(out_t *o, const char *k, long v)
{
    if (v < 0)
        put(o, " %s=-", k);
    else
        put(o, " %s=%ld", k, v);
}

static void put_x(out_t *o, const char *k, long v)
{
    if (v < 0)
        put(o, " %s=-", k);
    else
        put(o, " %s=0x%02lx", k, v);
}

static void put_d(out_t *o, const char *k, double v, int prec)
{
    if (isnan(v))
        put(o, " %s=-", k);
    else
        put(o, " %s=%.*f", k, prec, v);
}

/* A value is one token: a space would split it. */
static void put_s(out_t *o, const char *k, const char *v)
{
    if (!v || !*v) {
        put(o, " %s=-", k);
        return;
    }
    put(o, " %s=", k);
    for (const char *p = v; *p && !o->over; p++)
        put(o, "%c", (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ? '_' : *p);
}

static void prefix(out_t *o, const char *type, const senslog_ctx_t *c, double mono)
{
    put(o, "%s v=%d boot=%s mono=%.1f", type, SENSLOG_V, c->boot[0] ? c->boot : "-", mono);
}

static int done(out_t *o)
{
    return o->over ? -1 : (int)o->off;
}

/* ------------------------------------------------------------ samples */

void senslog_sample_clear(senslog_sample_t *s)
{
    memset(s, 0, sizeof(*s));
    s->hold = s->fire_ok = s->armed = -1;
    s->faults = s->sw = -1;
    s->down_raw = s->up_raw = -1;
    s->down_c = s->up_c = s->chassis_c = s->soc_c = NAN;
    s->psu_raw = s->tec_raw = s->throttle = -1;
    s->exhaust_pwm = s->intake_pwm = s->aa_pwm = -1;
    s->exhaust_ns = s->intake1_ns = s->intake2_ns = s->aa_us = -1;
    s->exhaust_rpm = s->intake1_rpm = s->intake2_rpm = s->aa_rpm = NAN;
    s->purge = s->purge_cur = s->pump = s->heater_pwm = s->tec_on = -1;
    s->hv = s->hv_v = s->lon = s->pgood = -1;
    s->beam = s->beam_dig = s->beam_base = s->beam_n = s->head_flags = -1;
    for (int i = 0; i < 4; i++)
        s->ir[i] = -1;
    s->hall = -1;
    s->cpu_pct = s->mem_pct = s->tick_ms = NAN;
    s->data_free_mb = -1;
}

static void put_flag(out_t *o, const char *k, int v)
{
    put_l(o, k, v < 0 ? -1 : !!v);
}

static void put_ir(out_t *o, const char *k, const long ir[4])
{
    if (ir[0] < 0 || ir[1] < 0 || ir[2] < 0 || ir[3] < 0)
        put(o, " %s=-", k);
    else
        put(o, " %s=%ld,%ld,%ld,%ld", k, ir[0], ir[1], ir[2], ir[3]);
}

int senslog_fmt_beat(char *buf, size_t len, const senslog_ctx_t *c,
                     const senslog_sample_t *s)
{
    out_t o = {buf, len, 0, 0};
    prefix(&o, "beat", c, s->mono);
    put_s(&o, "ph", s->phase);
    put_s(&o, "vd", s->verdict);
    put_flag(&o, "hold", s->hold);
    put_flag(&o, "fire", s->fire_ok);
    put_flag(&o, "arm", s->armed);
    put_s(&o, "kst", s->kst);
    put_l(&o, "flt", s->faults);
    put_x(&o, "sw", s->sw);
    put_d(&o, "down", s->down_c, 2);
    put_d(&o, "up", s->up_c, 2);
    put_l(&o, "down_raw", s->down_raw);
    put_l(&o, "up_raw", s->up_raw);
    put_d(&o, "chassis", s->chassis_c, 1);
    put_d(&o, "soc", s->soc_c, 1);
    put_l(&o, "psu_raw", s->psu_raw);
    put_l(&o, "tec_raw", s->tec_raw);
    put_l(&o, "thr", s->throttle);
    put_l(&o, "exhaust_pwm", s->exhaust_pwm);
    put_l(&o, "exhaust_ns", s->exhaust_ns);
    put_d(&o, "exhaust_rpm", s->exhaust_rpm, 0);
    put_l(&o, "intake_pwm", s->intake_pwm);
    put_l(&o, "intake1_ns", s->intake1_ns);
    put_d(&o, "intake1_rpm", s->intake1_rpm, 0);
    put_l(&o, "intake2_ns", s->intake2_ns);
    put_d(&o, "intake2_rpm", s->intake2_rpm, 0);
    put_l(&o, "aa_pwm", s->aa_pwm);
    put_l(&o, "aa_us", s->aa_us);
    put_d(&o, "aa_rpm", s->aa_rpm, 0);
    put_l(&o, "purge", s->purge);
    put_l(&o, "purge_cur", s->purge_cur);
    put_l(&o, "pump", s->pump);
    put_l(&o, "heater_pwm", s->heater_pwm);
    put_l(&o, "tec_on", s->tec_on);
    put_l(&o, "hv", s->hv);
    put_l(&o, "hv_v", s->hv_v);
    put_l(&o, "lon", s->lon);
    put_l(&o, "pgood", s->pgood);
    put_l(&o, "beam", s->beam);
    put_l(&o, "beam_dig", s->beam_dig);
    put_l(&o, "beam_base", s->beam_base);
    put_l(&o, "beam_n", s->beam_n);
    put_x(&o, "head_flags", s->head_flags);
    put_ir(&o, "ir", s->ir);
    put_l(&o, "hall", s->hall);
    put_d(&o, "cpu", s->cpu_pct, 0);
    put_d(&o, "mem", s->mem_pct, 0);
    put_d(&o, "tick_ms", s->tick_ms, 0);
    put_l(&o, "data_free_mb", s->data_free_mb);
    put(&o, " drops=%lu", s->drops);
    return done(&o);
}

int senslog_fmt_flow(char *buf, size_t len, const senslog_ctx_t *c,
                     double mono, const senslog_flow_t *f)
{
    out_t o = {buf, len, 0, 0};
    prefix(&o, "flow", c, mono);
    put_s(&o, "src", f->src);
    put_s(&o, "verdict", f->verdict);
    if (f->trials > 0)
        put(&o, " trial=%d/%d", f->trial, f->trials);
    put_d(&o, "rise", f->rise, 2);
    put_d(&o, "rise_raw", f->rise_raw, 2);
    put_d(&o, "laser", f->laser_c, 2);
    put_d(&o, "limit", f->limit_c, 2);
    put_l(&o, "duty", f->duty_pct);
    put_d(&o, "window_s", f->window_s, 0);
    put_d(&o, "base_down", f->base_down, 2);
    put_d(&o, "base_up", f->base_up, 2);
    put_d(&o, "rest_dt", isnan(f->base_down) || isnan(f->base_up)
                         ? NAN : f->base_down - f->base_up, 2);
    put_d(&o, "end_down", f->end_down, 2);
    put_d(&o, "end_up", f->end_up, 2);
    put_d(&o, "dt", f->dt, 2);
    put_d(&o, "hv_dose", f->hv_dose, 0);
    return done(&o);
}

static void iso_utc(double t, char *buf, size_t len)
{
    time_t s = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&s, &tm) || !strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm))
        snprintf(buf, len, "-");
}

int senslog_fmt_clock(char *buf, size_t len, const senslog_ctx_t *c,
                      double mono, double from, double to)
{
    char a[32], b[32];
    iso_utc(from, a, sizeof(a));
    iso_utc(to, b, sizeof(b));
    out_t o = {buf, len, 0, 0};
    prefix(&o, "clock", c, mono);
    put(&o, " from=%s to=%s", a, b);
    return done(&o);
}

int senslog_clock_stepped(double last_off, double off)
{
    return fabs(off - last_off) > 2.0;
}

int senslog_fmt_kv(char *buf, size_t len, const char *type,
                   const senslog_ctx_t *c, double mono, const char *why,
                   const char *body)
{
    out_t o = {buf, len, 0, 0};
    prefix(&o, type, c, mono);
    put_s(&o, "why", why);
    if (body && *body)
        put(&o, " %s", body);
    return done(&o);
}

/* ------------------------------------------------------------ the job */

static void stat_add(senslog_stat_t *st, double v)
{
    if (isnan(v))
        return;
    if (!st->n || v < st->min)
        st->min = v;
    if (!st->n || v > st->max)
        st->max = v;
    st->sum += v;
    st->n++;
}

static void put_stat(out_t *o, const char *k, const senslog_stat_t *st, int prec)
{
    if (!st->n)
        put(o, " %s=-", k);
    else
        put(o, " %s=%.*f/%.*f/%.*f", k, prec, st->min, prec,
            st->sum / (double)st->n, prec, st->max);
}

static int in_session(const senslog_sample_t *s)
{
    return !strcmp(s->phase, "run") || !strcmp(s->phase, "warm-up");
}

static int in_cooldown(const senslog_sample_t *s)
{
    return !strcmp(s->phase, "smoke") || !strcmp(s->phase, "thermal");
}

void senslog_jobs_init(senslog_jobs_t *j)
{
    memset(j, 0, sizeof(*j));
}

static void job_start(senslog_jobs_t *j, const senslog_sample_t *s)
{
    senslog_job_t *c = &j->cur;
    memset(c, 0, sizeof(*c));
    c->seq = ++j->seq;
    c->start_mono = s->mono;
    c->down_start = s->down_c;
    c->up_start = s->up_c;
    c->down_end = c->up_end = c->down_last = c->up_last = NAN;
    c->flow_worst = NAN;
    for (int i = 0; i < 4; i++)
        c->ir_base[i] = c->ir_peak[i] = -1;
    c->beam_peak = -1;
    j->active = 1;
    j->in_cooldown = 0;
}

static void job_finish(senslog_jobs_t *j, const senslog_sample_t *s, const char *end)
{
    j->cur.end_mono = s->mono;
    j->cur.end = end;
    j->cur.down_last = s->down_c;
    j->cur.up_last = s->up_c;
    j->done = j->cur;
    j->active = 0;
    j->in_cooldown = 0;
}

static void verdict_seen(senslog_job_t *c, const char *v)
{
    if (!v[0] || !strcmp(v, "OK"))
        return;
    size_t vl = strlen(v);
    for (const char *p = c->verdicts; *p; ) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == vl && !strncmp(p, v, n))
            return;
        if (!e)
            break;
        p = e + 1;
    }
    size_t used = strlen(c->verdicts);
    if (used + vl + 2 < sizeof(c->verdicts))
        snprintf(c->verdicts + used, sizeof(c->verdicts) - used, "%s%s",
                 used ? "," : "", v);
}

static void job_accumulate(senslog_jobs_t *j, const senslog_sample_t *s)
{
    senslog_job_t *c = &j->cur;
    if (in_session(s)) {
        c->run_s++;
        c->down_end = s->down_c;
        c->up_end = s->up_c;
        if (s->armed > 0)
            c->armed_s++;
        int lit = s->lon > 0;
        if (lit) {
            c->lit_s++;
            if (s->hv >= 0)
                c->hv_dose += (double)s->hv;
            if (s->hv >= SENSLOG_HV_SAT)
                c->hv_sat_s++;
            if (s->beam >= 0 && s->beam > c->beam_peak)
                c->beam_peak = s->beam;
        } else if (s->beam >= 0) {
            stat_add(&c->beam_dark, (double)s->beam);
        }
        int have_ir = s->ir[0] >= 0 && s->ir[1] >= 0 && s->ir[2] >= 0 && s->ir[3] >= 0;
        if (have_ir) {
            if (c->ir_base[0] < 0)
                memcpy(c->ir_base, s->ir, sizeof(c->ir_base));
            for (int i = 0; i < 4; i++)
                if (s->ir[i] > c->ir_peak[i])
                    c->ir_peak[i] = s->ir[i];
        }
        if (!strcmp(s->phase, "run") && s->mono - c->start_mono >= SENSLOG_FAN_GRACE_S) {
            stat_add(&c->fan[0], s->exhaust_rpm);
            stat_add(&c->fan[1], s->intake1_rpm);
            stat_add(&c->fan[2], s->intake2_rpm);
            stat_add(&c->fan[3], s->aa_rpm);
            stat_add(&c->fan[4], s->purge_cur >= 0 ? (double)s->purge_cur : NAN);
        }
    } else {
        c->cool_s++;
        j->in_cooldown = 1;
    }
    stat_add(&c->down, s->down_c);
    stat_add(&c->up, s->up_c);
    stat_add(&c->chassis, s->chassis_c);
    stat_add(&c->soc, s->soc_c);
    stat_add(&c->psu, s->psu_raw >= 0 ? (double)s->psu_raw : NAN);
    if (s->throttle > 0)
        c->thr_s++;
    verdict_seen(c, s->verdict);
}

int senslog_job_feed(senslog_jobs_t *j, const senslog_sample_t *s)
{
    int finished = 0;
    if (j->active) {
        if (in_session(s) && j->in_cooldown) {
            job_finish(j, s, "new-session");
            finished = 1;
        } else if (!in_session(s) && !in_cooldown(s)) {
            job_finish(j, s, !strcmp(s->phase, "idle") ? "idle" : s->phase);
            finished = 1;
        }
    }
    if (!j->active && in_session(s))
        job_start(j, s);
    if (j->active)
        job_accumulate(j, s);
    return finished;
}

void senslog_job_flow(senslog_jobs_t *j, double rise)
{
    if (!j->active)
        return;
    j->cur.flow_checks++;
    if (!isnan(rise) && (isnan(j->cur.flow_worst) || rise > j->cur.flow_worst))
        j->cur.flow_worst = rise;
}

int senslog_fmt_job(char *buf, size_t len, const senslog_ctx_t *c,
                    double mono, const senslog_job_t *job)
{
    out_t o = {buf, len, 0, 0};
    prefix(&o, "job", c, mono);
    put(&o, " seq=%lu start=%.1f", job->seq, job->start_mono);
    put_s(&o, "end", job->end);
    put_l(&o, "run_s", job->run_s);
    put_l(&o, "cool_s", job->cool_s);
    put_l(&o, "armed_s", job->armed_s);
    put_l(&o, "lit_s", job->lit_s);
    put_d(&o, "hv_dose", job->hv_dose, 0);
    put_d(&o, "hv_lit_mean", job->lit_s ? job->hv_dose / (double)job->lit_s : NAN, 0);
    put_l(&o, "hv_sat_s", job->hv_sat_s);
    put_d(&o, "down_start", job->down_start, 2);
    put_d(&o, "up_start", job->up_start, 2);
    put_d(&o, "down_end", job->down_end, 2);
    put_d(&o, "up_end", job->up_end, 2);
    put_d(&o, "down_last", job->down_last, 2);
    put_d(&o, "up_last", job->up_last, 2);
    put_stat(&o, "down", &job->down, 2);
    put_stat(&o, "up", &job->up, 2);
    put_stat(&o, "chassis", &job->chassis, 1);
    put_stat(&o, "soc", &job->soc, 1);
    put_stat(&o, "psu_raw", &job->psu, 0);
    put_l(&o, "thr_s", job->thr_s);
    put_stat(&o, "exhaust_rpm", &job->fan[0], 0);
    put_stat(&o, "intake1_rpm", &job->fan[1], 0);
    put_stat(&o, "intake2_rpm", &job->fan[2], 0);
    put_stat(&o, "aa_rpm", &job->fan[3], 0);
    put_stat(&o, "purge_cur", &job->fan[4], 0);
    put_ir(&o, "ir_base", job->ir_base);
    put_ir(&o, "ir_peak", job->ir_peak);
    put_stat(&o, "beam_dark", &job->beam_dark, 0);
    put_l(&o, "beam_peak", job->beam_peak);
    put_d(&o, "beam_rise", job->beam_peak >= 0 && job->beam_dark.n
                           ? (double)job->beam_peak - job->beam_dark.sum / (double)job->beam_dark.n
                           : NAN, 0);
    put_l(&o, "flow_checks", job->flow_checks);
    put_d(&o, "flow_worst", job->flow_worst, 2);
    put_s(&o, "verdicts", job->verdicts);
    return done(&o);
}

/* -------------------------------------------------- the flight recorder */

void senslog_ring_push(senslog_ring_t *r, const senslog_sample_t *s)
{
    r->s[r->n % SENSLOG_FR_N] = *s;
    r->n++;
}

typedef struct {
    const char *name;
    char kind;              /* l long, d double, s string */
    int prec;
    size_t off;
} fr_ch_t;

#define CH_L(n, f)    { n, 'l', 0, offsetof(senslog_sample_t, f) }
#define CH_D(n, f, p) { n, 'd', p, offsetof(senslog_sample_t, f) }
#define CH_S(n, f)    { n, 's', 0, offsetof(senslog_sample_t, f) }

static const fr_ch_t fr_ch[] = {
    CH_S("ph", phase), CH_S("vd", verdict),
    CH_D("down", down_c, 2), CH_D("up", up_c, 2),
    CH_L("down_raw", down_raw), CH_L("up_raw", up_raw),
    CH_L("heater_pwm", heater_pwm),
    CH_L("hv", hv), CH_L("lon", lon), CH_L("beam", beam),
    CH_L("ir1", ir[0]), CH_L("ir2", ir[1]), CH_L("ir3", ir[2]), CH_L("ir4", ir[3]),
    CH_D("exhaust_rpm", exhaust_rpm, 0), CH_D("intake1_rpm", intake1_rpm, 0),
    CH_D("intake2_rpm", intake2_rpm, 0), CH_D("aa_rpm", aa_rpm, 0),
    CH_L("purge_cur", purge_cur),
    CH_D("soc", soc_c, 1), CH_D("cpu", cpu_pct, 0),
};

int senslog_fr_channels(void)
{
    return (int)(sizeof(fr_ch) / sizeof(*fr_ch));
}

int senslog_fmt_fr_head(char *buf, size_t len, const senslog_ctx_t *c,
                        double mono, unsigned long ev, double at,
                        const char *msg)
{
    out_t o = {buf, len, 0, 0};
    prefix(&o, "fr", c, mono);
    put(&o, " ev=%lu at=%.1f pre=%d post=%d", ev, at, SENSLOG_FR_PRE_S, SENSLOG_FR_POST_S);
    put(&o, " msg=");
    for (const char *p = msg ? msg : ""; *p && !o.over; p++)
        put(&o, "%c", (*p == '\n' || *p == '\r') ? ' ' : *p);
    return done(&o);
}

int senslog_fmt_fr_channel(char *buf, size_t len, const senslog_ctx_t *c,
                           double mono, unsigned long ev,
                           const senslog_ring_t *r, double from, double to,
                           int ch)
{
    if (ch < 0 || ch >= senslog_fr_channels())
        return -1;
    const fr_ch_t *d = &fr_ch[ch];
    unsigned long have = r->n < SENSLOG_FR_N ? r->n : SENSLOG_FR_N;
    unsigned long first = r->n - have;
    double t0 = NAN, t1 = NAN;
    int count = 0;
    for (unsigned long k = first; k < r->n; k++) {
        const senslog_sample_t *s = &r->s[k % SENSLOG_FR_N];
        if (s->mono < from || s->mono > to)
            continue;
        if (!count)
            t0 = s->mono;
        t1 = s->mono;
        count++;
    }
    out_t o = {buf, len, 0, 0};
    prefix(&o, "frc", c, mono);
    put(&o, " ev=%lu ch=%s n=%d", ev, d->name, count);
    put_d(&o, "t0", t0, 1);
    put_d(&o, "t1", t1, 1);
    put(&o, " vals=");
    int first_val = 1;
    for (unsigned long k = first; k < r->n && !o.over; k++) {
        const senslog_sample_t *s = &r->s[k % SENSLOG_FR_N];
        if (s->mono < from || s->mono > to)
            continue;
        const char *base = (const char *)s + d->off;
        if (!first_val)
            put(&o, ",");
        first_val = 0;
        if (d->kind == 'l') {
            long v = *(const long *)base;
            if (v < 0)
                put(&o, "-");
            else
                put(&o, "%ld", v);
        } else if (d->kind == 'd') {
            double v = *(const double *)base;
            if (isnan(v))
                put(&o, "-");
            else
                put(&o, "%.*f", d->prec, v);
        } else {
            put(&o, "%s", *base ? base : "-");
        }
    }
    return done(&o);
}

/* -------------------------------------------------------- the cadence */

int senslog_beat_due(const senslog_sample_t *s, double last_beat,
                     const char *last_phase)
{
    if (last_beat < 0)
        return 1;
    if (strcmp(s->phase, last_phase ? last_phase : ""))
        return 1;
    int active = strcmp(s->phase, "idle") || s->armed > 0 || !strcmp(s->kst, "running");
    double every = active ? SENSLOG_BEAT_RUN_S : SENSLOG_BEAT_IDLE_S;
    /* Half a second of slack: the samples land on a one-second grid. */
    return s->mono - last_beat >= every - 0.5;
}
