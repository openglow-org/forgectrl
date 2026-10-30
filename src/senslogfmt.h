/*
 * senslogfmt.h - the sensors logger's records (see senslog.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The pure half of the sensors logger: the record formats, the job
 * summary built from the 1 Hz samples, the flight recorder's ring, and
 * the cadence. No I/O: senslog.c reads the machine and emits the lines,
 * the host test drives these against fixed samples.
 *
 * A record is one line: its type, then key=value pairs separated by one
 * space. Every record carries v (the schema version), boot (the first 8
 * hex digits of the kernel's boot id) and mono (seconds since boot), so
 * lines written before the clock was set can still be ordered and
 * re-dated. A value that could not be read is "-". Keys are only ever
 * added; a key renamed or removed bumps SENSLOG_V. A key named msg, when
 * present, is the last one and runs to the end of the line. The schema is
 * the docs site's (technical/forgefirm/logging.md, "The sensors logger").
 */
#ifndef FORGECTRL_SENSLOGFMT_H
#define FORGECTRL_SENSLOGFMT_H

#include <stddef.h>

#define SENSLOG_V           1
#define SENSLOG_BEAT_RUN_S  10.0    /* a session open, cooling down, or diagnostics */
#define SENSLOG_BEAT_IDLE_S 60.0
#define SENSLOG_FR_PRE_S    120     /* the flight recorder: seconds before the event */
#define SENSLOG_FR_POST_S   30      /* ... and after it */
#define SENSLOG_FR_N        (SENSLOG_FR_PRE_S + SENSLOG_FR_POST_S + 2)
#define SENSLOG_FAN_GRACE_S 15.0    /* a job's fan statistics skip the spin-up */
#define SENSLOG_HV_SAT      1023    /* pic/hv_current at its converter's top */

/* One second of the machine. Integers are raw counts or states, -1 when
 * unread; doubles are converted values, NAN when unread. */
typedef struct {
    double mono;
    /* the cooling engine's view */
    char phase[12], verdict[12];
    int hold, fire_ok, armed;
    /* the kernel */
    char kst[12];
    long faults, sw;
    /* coolant and board temperatures */
    long down_raw, up_raw;
    double down_c, up_c;
    double chassis_c, soc_c;
    long psu_raw, tec_raw, throttle;
    /* airflow: commanded duty, tach period, speed */
    long exhaust_pwm, intake_pwm, aa_pwm;
    long exhaust_ns, intake1_ns, intake2_ns, aa_us;
    double exhaust_rpm, intake1_rpm, intake2_rpm, aa_rpm;
    long purge, purge_cur, pump, heater_pwm, tec_on;
    /* the tube and the head */
    long hv, hv_v, lon, pgood;
    /* the head's beam reading, its comparator, the head MCU's own
     * baseline and its samples in the last window, its flag register */
    long beam, beam_dig, beam_base, beam_n, head_flags;
    long ir[4];
    long hall;
    /* the system */
    double cpu_pct, mem_pct, tick_ms;
    long data_free_mb;
    unsigned long drops;
} senslog_sample_t;

/* A sample with every value unread. */
void senslog_sample_clear(senslog_sample_t *s);

/* The common prefix every record starts with. boot is 8 hex digits. */
typedef struct {
    char boot[9];
} senslog_ctx_t;

/* The records. Each returns the line length, or -1 if it does not fit. */
int senslog_fmt_beat(char *buf, size_t len, const senslog_ctx_t *c,
                     const senslog_sample_t *s);

/* A flow verdict: the engine's check (src "check") or a calibration
 * trial (src "cal", trial i of n). */
typedef struct {
    const char *src;
    const char *verdict;
    double rise, rise_raw, laser_c, limit_c;
    long duty_pct;
    double window_s;
    double base_down, base_up, end_down, end_up, dt;
    double hv_dose;
    int trial, trials;
} senslog_flow_t;
int senslog_fmt_flow(char *buf, size_t len, const senslog_ctx_t *c,
                     double mono, const senslog_flow_t *f);

/* The clock stepped: from and to are wall-clock seconds (UTC). */
int senslog_fmt_clock(char *buf, size_t len, const senslog_ctx_t *c,
                      double mono, double from, double to);
/* The step of (wall - mono) past which the clock counts as set. */
int senslog_clock_stepped(double last_off, double off);

/* The machine's inventory and the settings in force: why is boot,
 * clock, daily or change; body is the record's key=value pairs. */
int senslog_fmt_kv(char *buf, size_t len, const char *type,
                   const senslog_ctx_t *c, double mono, const char *why,
                   const char *body);

/* ---------------------------------------------------------- the job */

typedef struct {
    double sum, min, max;
    long n;
} senslog_stat_t;

/* One job: the run session from its opening to the machine's return to
 * idle, its cooldown included (the loop's peak often comes after the
 * beam stops). */
typedef struct {
    unsigned long seq;
    double start_mono, end_mono;
    const char *end;                /* idle, new-session, diag */
    long run_s, cool_s, armed_s, lit_s, hv_sat_s, thr_s;
    double hv_dose;
    double down_start, up_start, down_end, up_end, down_last, up_last;
    senslog_stat_t down, up, chassis, soc, psu;
    senslog_stat_t fan[5];          /* exhaust, intake 1, intake 2, air assist rpm; purge current */
    long ir_base[4], ir_peak[4];
    senslog_stat_t beam_dark;
    long beam_peak;
    int flow_checks;
    double flow_worst;
    char verdicts[96];              /* the distinct verdicts other than OK */
} senslog_job_t;

typedef struct {
    int active, in_cooldown;
    unsigned long seq;
    senslog_job_t cur;
    senslog_job_t done;             /* the job senslog_job_feed last finished */
} senslog_jobs_t;

void senslog_jobs_init(senslog_jobs_t *j);
/* Feed a sample. Returns 1 when a job finished on it: the finished job is
 * in j->done, and a session opening on the same sample has begun. */
int senslog_job_feed(senslog_jobs_t *j, const senslog_sample_t *s);
/* A flow verdict lands in the open job. */
void senslog_job_flow(senslog_jobs_t *j, double rise);
int senslog_fmt_job(char *buf, size_t len, const senslog_ctx_t *c,
                    double mono, const senslog_job_t *job);

/* --------------------------------------------------- the flight recorder */

typedef struct {
    senslog_sample_t s[SENSLOG_FR_N];
    unsigned long n;                /* samples pushed, ever */
} senslog_ring_t;

void senslog_ring_push(senslog_ring_t *r, const senslog_sample_t *s);
/* The number of channels a dump writes, and one channel's line: every
 * sample in the ring with mono in [from, to]. ch is 0..count-1. */
int senslog_fr_channels(void);
int senslog_fmt_fr_head(char *buf, size_t len, const senslog_ctx_t *c,
                        double mono, unsigned long ev, double at,
                        const char *msg);
int senslog_fmt_fr_channel(char *buf, size_t len, const senslog_ctx_t *c,
                           double mono, unsigned long ev,
                           const senslog_ring_t *r, double from, double to,
                           int ch);

/* -------------------------------------------------------- the cadence */

/* A beat is due when the interval for the sample's state has passed
 * since the last one, or the engine's phase changed. */
int senslog_beat_due(const senslog_sample_t *s, double last_beat,
                     const char *last_phase);

#endif
