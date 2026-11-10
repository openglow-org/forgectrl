/*
 * senslog_test.c - host unit test for the sensors logger's records
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The records are a contract with whoever reads a bundle: the bundle
 * reader (tools/senslog_read.py) parses tests/senslog_records.txt, and
 * this test proves the formatters still write exactly those lines. A
 * format change fails here until the golden file is rewritten
 * (SENSLOG_GOLDEN=write), and then fails the reader's test until the
 * reader follows it. Beside the contract:
 *
 *   A. the widest beat, flight-recorder channel and job fit one log
 *      datagram; an unread value is "-"
 *   B. a job runs from the session's opening to the return to idle, its
 *      cooldown included: the lit seconds, the dose, the coolant's start,
 *      end and peak, the fans past their spin-up, the lid IR, the beam's
 *      dark level and peak, the verdicts other than OK
 *   C. a session opening during a cooldown ends the job before it
 *   D. the flight recorder's channel holds the window's samples only
 *   E. the beat: every 60 s at idle, every 10 s in a session, at once on
 *      a phase change
 *   F. the clock: a step past 2 s is a step, a drift is not
 *   G. the flight recorder's budget: six dumps an hour for the alarm tier
 *      and six for the rest, so a flapping warning never costs an alarm its
 *      dump
 *
 * And the logger itself (senslog.c by inclusion, the machine's reads and
 * the log line stood in for):
 *
 *   H. a stop writes the dump an event still waits for, once, with the
 *      samples the ring holds
 *   I. a switch device that could not be read is "-", never a word of zeros
 */
/* The logger's own context is named apart from this file's. */
#define ctx logger_ctx
#include "../src/senslog.c"
#undef ctx

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

#define GOLDEN  "tests/senslog_records.txt"
#define DGRAM   1900        /* a line and fflog's header within its 2048-byte datagram */

static const senslog_ctx_t ctx = { "fcce7554" };

/* A machine at rest with the tube dark: the bench reference's readings. */
static senslog_sample_t rest(double mono)
{
    senslog_sample_t s;
    senslog_sample_clear(&s);
    s.mono = mono;
    snprintf(s.phase, sizeof(s.phase), "idle");
    snprintf(s.verdict, sizeof(s.verdict), "OK");
    s.hold = 0; s.fire_ok = 1; s.armed = 0;
    snprintf(s.kst, sizeof(s.kst), "idle");
    s.faults = 0; s.sw = 0x28;
    s.down_raw = 694; s.up_raw = 695; s.down_c = 21.43; s.up_c = 21.38;
    s.chassis_c = 26.6; s.soc_c = 44.5; s.psu_raw = 559; s.tec_raw = 1023; s.throttle = 0;
    s.exhaust_pwm = 0; s.intake_pwm = 0; s.aa_pwm = 204;
    s.exhaust_ns = 0; s.intake1_ns = 40380529; s.intake2_ns = 40282196; s.aa_us = 3685;
    s.exhaust_rpm = 0; s.intake1_rpm = 743; s.intake2_rpm = 745; s.aa_rpm = 2035;
    s.purge = 1; s.purge_cur = 623; s.pump = 1; s.heater_pwm = 0; s.tec_on = 0;
    s.hv = 0; s.hv_v = 0; s.lon = 0; s.pgood = 255;
    s.beam = 1834; s.beam_dig = 0; s.beam_base = 1825; s.beam_n = 6; s.head_flags = 0x08;
    s.ir[0] = 50; s.ir[1] = 49; s.ir[2] = 52; s.ir[3] = 54;
    s.hall = 0;
    s.cpu_pct = 12; s.mem_pct = 27; s.tick_ms = 31; s.data_free_mb = 2707; s.drops = 0;
    return s;
}

static senslog_sample_t at_phase(double mono, const char *phase)
{
    senslog_sample_t s = rest(mono);
    snprintf(s.phase, sizeof(s.phase), "%s", phase);
    return s;
}

/* ------------------------------------------------------------ golden */

static char golden[16384];
static size_t golden_n;

static void add_line(const char *line, int n)
{
    if (n < 0) {
        printf("  FAIL: a golden record did not fit its buffer\n");
        failures++;
        return;
    }
    if (golden_n + (size_t)n + 2 < sizeof(golden)) {
        memcpy(golden + golden_n, line, (size_t)n);
        golden_n += (size_t)n;
        golden[golden_n++] = '\n';
        golden[golden_n] = '\0';
    }
}

static void golden_lines(void)
{
    char line[2048];
    senslog_sample_t s = rest(1234.5);
    add_line(line, senslog_fmt_beat(line, sizeof(line), &ctx, &s));

    senslog_sample_t u;
    senslog_sample_clear(&u);
    u.mono = 1235.0;
    snprintf(u.phase, sizeof(u.phase), "diag");
    add_line(line, senslog_fmt_beat(line, sizeof(line), &ctx, &u));

    add_line(line, senslog_fmt_clock(line, sizeof(line), &ctx, 41.0, 41.0, 1793385000.0));
    add_line(line, senslog_fmt_kv(line, sizeof(line), "inv", &ctx, 41.0, "clock",
                                  "image=v0.0.11 kernel=6.12.20 soc=i.MX6DL head_id=044c head_fw=0bb13047 model=basic tec=0"));
    add_line(line, senslog_fmt_kv(line, sizeof(line), "cfg", &ctx, 41.0, "clock",
                                  "cool_temp_max=33 cool_aa_offset_counts=0 xy_microsteps=-"));

    senslog_ring_t ring = {0};
    for (int i = 0; i < 5; i++) {
        senslog_sample_t r = rest(3000.0 + i);
        r.down_c = 22.0 + i * 0.5;
        senslog_ring_push(&ring, &r);
    }
    add_line(line, senslog_fmt_fr_head(line, sizeof(line), &ctx, 3031.0, 1, 3001.0,
                                       "verdict OVERTEMP: coolant 33.4 C over 33 C limit - hold until 31 C"));
    add_line(line, senslog_fmt_fr_channel(line, sizeof(line), &ctx, 3031.0, 1, &ring, 2999.5, 3003.5, 2));
    add_line(line, senslog_fmt_fr_channel(line, sizeof(line), &ctx, 3031.0, 1, &ring, 2999.5, 3003.5, 1));
}

/* Section B's job, which the golden file also carries. */
static senslog_job_t scripted_job(senslog_jobs_t *j, int *finished_at)
{
    senslog_jobs_init(j);
    double t = 5000.0;
    *finished_at = -1;
    int k = 0;
    senslog_job_feed(j, &(senslog_sample_t){ .mono = t++, .phase = "idle" });
    for (int i = 0; i < 60; i++, k++) {
        senslog_sample_t r = at_phase(t++, "run");
        r.armed = i >= 5;
        r.down_c = 21.0 + i * 0.1;
        r.up_c = 20.9 + i * 0.05;
        /* The fans spin up for the first seconds: those never count. */
        r.exhaust_rpm = i < 10 ? 500 : 3000 + (i % 3) * 30;
        r.intake1_rpm = i < 10 ? 400 : 2400;
        r.aa_rpm = i < 10 ? 2000 : 7800;
        if (i >= 20 && i < 40) {
            r.lon = 240;
            r.hv = i < 30 ? 1023 : 500;
            r.beam = 1900 + i;
            r.ir[3] = 80;
        } else {
            r.beam = 1830 + (i % 2) * 4;
        }
        snprintf(r.verdict, sizeof(r.verdict), "%s", i == 45 ? "OVERTEMP" : "OK");
        if (senslog_job_feed(j, &r))
            *finished_at = k;
    }
    for (int i = 0; i < 15; i++, k++) {
        senslog_sample_t r = at_phase(t++, "smoke");
        r.down_c = 27.0 - i * 0.05;
        r.up_c = 24.5;
        if (senslog_job_feed(j, &r))
            *finished_at = k;
    }
    for (int i = 0; i < 30; i++, k++) {
        senslog_sample_t r = at_phase(t++, "thermal");
        r.down_c = 26.0 - i * 0.1;
        r.up_c = 25.0 - i * 0.05;       /* the loop's peak upstream: after the beam stopped */
        if (i == 0)
            r.up_c = 25.2;
        if (senslog_job_feed(j, &r))
            *finished_at = k;
    }
    senslog_sample_t idle = at_phase(t++, "idle");
    idle.down_c = 23.0;
    idle.up_c = 22.8;
    if (senslog_job_feed(j, &idle))
        *finished_at = k;
    return j->done;
}

static void golden_check(void)
{
    printf("golden: the records as the reader parses them\n");
    golden_lines();
    senslog_jobs_t j;
    int fin;
    senslog_job_t job = scripted_job(&j, &fin);
    char line[2048];
    add_line(line, senslog_fmt_job(line, sizeof(line), &ctx, job.end_mono, &job));

    const char *mode = getenv("SENSLOG_GOLDEN");
    if (mode && !strcmp(mode, "write")) {
        FILE *f = fopen(GOLDEN, "w");
        CHECK(f && fputs(golden, f) >= 0 && fclose(f) == 0, "the golden file rewritten");
        return;
    }
    static char want[16384];
    FILE *f = fopen(GOLDEN, "r");
    size_t n = f ? fread(want, 1, sizeof(want) - 1, f) : 0;
    if (f)
        fclose(f);
    want[n] = '\0';
    int same = n == golden_n && !memcmp(want, golden, n);
    if (!same) {
        printf("  the formatters now write:\n%s", golden);
    }
    CHECK(same, "the formatters write exactly " GOLDEN " (a change rewrites it: SENSLOG_GOLDEN=write)");
}

/* ------------------------------------------------------------ sections */

static void widest(void)
{
    printf("A. the widest records fit one datagram; an unread value is \"-\"\n");
    char line[4096];
    senslog_sample_t s = rest(4294967.9);
    snprintf(s.phase, sizeof(s.phase), "warm-up");
    snprintf(s.verdict, sizeof(s.verdict), "OVERTEMP");
    snprintf(s.kst, sizeof(s.kst), "underrun");
    s.faults = 65535; s.sw = 0xffff;
    s.down_c = s.up_c = -273.15; s.chassis_c = s.soc_c = 125.0;
    long big = 4294967295L;
    s.exhaust_ns = s.intake1_ns = s.intake2_ns = s.aa_us = big;
    s.exhaust_rpm = s.intake1_rpm = s.intake2_rpm = s.aa_rpm = 99999;
    s.exhaust_pwm = s.intake_pwm = s.heater_pwm = 65535;
    s.cpu_pct = 100; s.mem_pct = 100; s.tick_ms = 99999; s.drops = 4294967295UL;
    int n = senslog_fmt_beat(line, sizeof(line), &ctx, &s);
    CHECK(n > 0 && n < DGRAM, "the widest beat fits a datagram");

    senslog_ring_t ring = {0};
    for (int i = 0; i < SENSLOG_FR_N + 10; i++) {
        senslog_sample_t r = s;
        r.mono = 100.0 + i;
        snprintf(r.verdict, sizeof(r.verdict), "CRITICAL");
        senslog_ring_push(&ring, &r);
    }
    int worst = 0;
    for (int c = 0; c < senslog_fr_channels(); c++) {
        int m = senslog_fmt_fr_channel(line, sizeof(line), &ctx, 1e6, 99999, &ring, 0, 1e9, c);
        if (m < 0 || m > worst)
            worst = m < 0 ? 99999 : m;
    }
    CHECK(worst > 0 && worst < DGRAM, "every flight-recorder channel of a full ring fits a datagram");

    senslog_job_t job;
    memset(&job, 0, sizeof(job));
    job.seq = 4294967295UL; job.start_mono = 4294967.9;
    snprintf(job.end, sizeof(job.end), "new-session");
    job.run_s = job.cool_s = job.armed_s = job.lit_s = job.hv_sat_s = job.thr_s = 4294967;
    job.hv_dose = 4.3e9;
    job.down_start = job.up_start = job.down_end = job.up_end = job.down_last = job.up_last = -273.15;
    senslog_stat_t st = { 1e6, -273.15, 99999, 1 };
    job.down = job.up = job.chassis = job.soc = job.psu = job.beam_dark = st;
    for (int i = 0; i < 5; i++)
        job.fan[i] = st;
    for (int i = 0; i < 4; i++)
        job.ir_base[i] = job.ir_peak[i] = 1023;
    job.beam_peak = 65535;
    snprintf(job.verdicts, sizeof(job.verdicts), "FIRE,CRASH,FLAME,BUMP,AIRFLOW,CRITICAL,SENSOR,OVERTEMP,COLD,WARMUP,EXT");
    n = senslog_fmt_job(line, sizeof(line), &ctx, 4294967.9, &job);
    CHECK(n > 0 && n < DGRAM, "the widest job fits a datagram");

    senslog_sample_t u;
    senslog_sample_clear(&u);
    n = senslog_fmt_beat(line, sizeof(line), &ctx, &u);
    CHECK(n > 0 && strstr(line, " down=- ") && strstr(line, " ir=- ") && strstr(line, " head_flags=- "),
          "an unread value is \"-\"");
}

static void job(void)
{
    printf("B. a job, from the session's opening to the return to idle\n");
    senslog_jobs_t j;
    int fin;
    senslog_job_t d = scripted_job(&j, &fin);
    CHECK(fin == 105 && !strcmp(d.end, "idle") && !j.active, "it ends on the first idle sample, and only then");
    CHECK(d.run_s == 60 && d.cool_s == 45 && d.armed_s == 55, "run, cooldown and armed seconds");
    CHECK(d.lit_s == 20 && d.hv_sat_s == 10 && d.hv_dose == 10 * 1023.0 + 10 * 500.0,
          "lit seconds, the converter's top counted, the dose");
    CHECK(fabs(d.down_start - 21.0) < 1e-9 && fabs(d.down_end - 26.9) < 1e-9 && fabs(d.up.max - 25.2) < 1e-9 &&
          fabs(d.down_last - 23.0) < 1e-9, "the coolant at the opening, at the session's end, its peak in the cooldown, at idle");
    CHECK(d.fan[0].n == 45 && d.fan[0].min >= 3000 && d.fan[1].min == 2400,
          "the fans counted past their spin-up only");
    CHECK(d.ir_base[3] == 54 && d.ir_peak[3] == 80, "the lid IR's baseline and peak");
    CHECK(d.beam_peak == 1939 && d.beam_dark.n == 40 && d.beam_dark.max == 1834,
          "the beam's peak lit, its dark level from the dark seconds");
    CHECK(!strcmp(d.verdicts, "OVERTEMP"), "the verdicts other than OK");

    printf("C. a session opening during a cooldown ends the job before it\n");
    senslog_jobs_init(&j);
    senslog_job_feed(&j, &(senslog_sample_t){ .mono = 1, .phase = "run" });
    senslog_job_feed(&j, &(senslog_sample_t){ .mono = 2, .phase = "smoke" });
    int f = senslog_job_feed(&j, &(senslog_sample_t){ .mono = 3, .phase = "run" });
    CHECK(f && !strcmp(j.done.end, "new-session") && j.done.seq == 1 && j.active && j.cur.seq == 2,
          "the first job ends new-session, the second has begun");
}

static void recorder(void)
{
    printf("D. the flight recorder's channel holds the window's samples only\n");
    senslog_ring_t ring = {0};
    for (int i = 0; i < 400; i++) {
        senslog_sample_t r = rest(i);
        r.hv = i;
        senslog_ring_push(&ring, &r);
    }
    char line[4096];
    int hv = -1;
    for (int c = 0; c < senslog_fr_channels(); c++) {
        senslog_fmt_fr_channel(line, sizeof(line), &ctx, 400, 1, &ring, 0, 1e9, c);
        if (strstr(line, " ch=hv "))
            hv = c;
    }
    senslog_fmt_fr_channel(line, sizeof(line), &ctx, 400, 1, &ring, 300, 310, hv);
    CHECK(strstr(line, " n=11 ") && strstr(line, " t0=300.0 ") && strstr(line, "vals=300,301,302,303,304,305,306,307,308,309,310"),
          "a window inside the ring: its samples, in order");
    senslog_fmt_fr_channel(line, sizeof(line), &ctx, 400, 1, &ring, 0, 1e9, hv);
    char want[32];
    snprintf(want, sizeof(want), " n=%d ", SENSLOG_FR_N);
    CHECK(strstr(line, want) != NULL, "a window past the ring's reach: what the ring still holds");
}

static void cadence(void)
{
    printf("E. the beat's cadence\n");
    senslog_sample_t s = rest(100.0);
    CHECK(senslog_beat_due(&s, -1, ""), "the first sample beats");
    s.mono = 159.0;
    CHECK(!senslog_beat_due(&s, 100.0, "idle"), "idle: not before 60 s");
    s.mono = 160.0;
    CHECK(senslog_beat_due(&s, 100.0, "idle"), "idle: at 60 s");
    s = at_phase(105.0, "run");
    CHECK(senslog_beat_due(&s, 100.0, "idle"), "a phase change beats at once");
    s.mono = 109.0;
    CHECK(!senslog_beat_due(&s, 100.0, "run"), "a session: not before 10 s");
    s.mono = 110.0;
    CHECK(senslog_beat_due(&s, 100.0, "run"), "a session: at 10 s");
    s = rest(110.0);
    s.armed = 1;
    CHECK(senslog_beat_due(&s, 100.0, "idle"), "armed at idle beats at the session's pace");
}

static void clock_step(void)
{
    printf("F. the clock\n");
    CHECK(!senslog_clock_stepped(1000.0, 1001.9), "a drift under 2 s is not a step");
    CHECK(senslog_clock_stepped(1000.0, 1793385000.0), "the clock set from 1970 is");
}

static void budget(void)
{
    printf("G. the flight recorder's budget\n");
    CHECK(senslog_fr_alarm("verdict CRASH: head accelerometer") && senslog_fr_alarm("controller silent while armed")
              && senslog_fr_alarm("stepper driver fault reported (mask 2)"),
          "a crash, a silent controller and a driver fault are of the alarm tier");
    CHECK(!senslog_fr_alarm("verdict AIRFLOW: exhaust") && !senslog_fr_alarm("SoC throttled: CPU cooling state 1")
              && !senslog_fr_alarm(NULL), "a pause-tier verdict and a throttle are not");
    senslog_fr_budget_t b;
    memset(&b, 0, sizeof(b));
    int took = 0;
    for (int i = 0; i < 10; i++)
        took += senslog_fr_admit(&b, 100.0 + i, 0);
    CHECK(took == SENSLOG_FR_PER_HOUR, "a flapping warning gets six dumps in its hour");
    CHECK(senslog_fr_admit(&b, 120.0, 1), "and an alarm after it still gets its own");
    CHECK(senslog_fr_admit(&b, 100.0 + 3600.0, 0), "an hour on, the warnings have their budget again");
}

/* ------------------------------------------------- the logger's stand-ins */

/* The sensors log: every line the logger writes, in order. */
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char logged[96][LINE_MAX_B];
static int nlogged;

void fflog_ch(int c, int prio, const char *fmt, ...)
{
    (void)c;
    (void)prio;
    pthread_mutex_lock(&log_mu);
    if (nlogged < (int)(sizeof(logged) / sizeof(*logged))) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(logged[nlogged++], sizeof(logged[0]), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&log_mu);
}

/* The lines that start with a record type, and the last of them. */
static int logged_of(const char *type, char *last, size_t len)
{
    int n = 0;
    size_t tl = strlen(type);
    pthread_mutex_lock(&log_mu);
    for (int i = 0; i < nlogged; i++)
        if (!strncmp(logged[i], type, tl) && logged[i][tl] == ' ') {
            n++;
            if (last && len)
                snprintf(last, len, "%.*s", (int)len - 1, logged[i]);
        }
    pthread_mutex_unlock(&log_mu);
    return n;
}

int fflog_channel(const char *name) { (void)name; return 0; }
unsigned long fflog_dropped(void) { return 0; }
void fflog(int prio, const char *fmt, ...) { (void)prio; (void)fmt; }
double airflow_rpm(long period, double units_per_second, int pulses_per_rev)
{
    (void)period; (void)units_per_second; (void)pulses_per_rev;
    return 0;
}
void cool_view(cool_view_t *v)
{
    memset(v, 0, sizeof(*v));
    snprintf(v->phase, sizeof(v->phase), "idle");
    snprintf(v->verdict, sizeof(v->verdict), "OK");
    v->fire_ok = 1;
}
double cool_coolant_c(long raw) { return (double)raw; }
double cool_tick_max_ms(void) { return 1; }
void cool_settings_in_force(char *buf, size_t len) { if (len) buf[0] = '\0'; }
void read_fw_version(char *buf, size_t len) { snprintf(buf, len, "test"); }
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }
void setup_machine_str(const char *key, char *buf, size_t len) { (void)key; if (len) buf[0] = '\0'; }
int setup_machine_bool(const char *key) { (void)key; return -1; }
double chassis_degc(void) { return -273.15; }
double soc_degc(void) { return -273.15; }
long soc_throttle_state(void) { return -1; }

static int sw_unread;                   /* the switch device cannot be read */
static unsigned long sw_word;
int machine_switch_read(unsigned long *bits)
{
    *bits = sw_unread ? 0 : sw_word;
    return sw_unread ? -1 : 0;
}

static void stop_dump(void)
{
    printf("H. a stop writes the dump an event still waits for\n");
    char line[LINE_MAX_B] = "";
    senslog_start();
    /* The thread's first second is in the ring once its first beat is out. */
    for (int i = 0; i < 300 && !logged_of("beat", NULL, 0); i++)
        usleep(10000);
    CHECK(logged_of("beat", NULL, 0) == 1, "the logger runs: its first beat is written");
    senslog_event("verdict FIRE: a stop inside the window");
    CHECK(logged_of("fr", NULL, 0) == 0, "the event's dump waits out its window");
    senslog_stop();
    CHECK(logged_of("fr", line, sizeof(line)) == 1 && strstr(line, " msg=verdict FIRE: a stop inside the window"),
          "the stop writes the dump, the event named");
    CHECK(logged_of("frc", line, sizeof(line)) == senslog_fr_channels() && !strstr(line, " n=0 "),
          "with every channel, and the samples the ring holds");
    senslog_stop();
    CHECK(logged_of("fr", NULL, 0) == 1, "once: a second stop writes nothing");
}

static void unread_switch(void)
{
    printf("I. a switch device that could not be read is \"-\"\n");
    char line[LINE_MAX_B];
    senslog_sample_t s;
    cpu_ctr_t cpu = {0};
    sw_unread = 1;
    sample(&s, &cpu);
    CHECK(senslog_fmt_beat(line, sizeof(line), &ctx, &s) > 0 && strstr(line, " sw=- "),
          "unread: sw=-, not a word that says the lid is open");
    sw_unread = 0;
    sw_word = 0x28;
    sample(&s, &cpu);
    CHECK(senslog_fmt_beat(line, sizeof(line), &ctx, &s) > 0 && strstr(line, " sw=0x28 "), "read: the word");
}

int main(void)
{
    golden_check();
    widest();
    job();
    recorder();
    cadence();
    clock_step();
    budget();
    stop_dump();
    unread_switch();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
