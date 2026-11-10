/*
 * flowrun.c - forgectrl: the coolant flow watch's thread and heater (see flowrun.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "flowrun.h"

#include "fflog.h"
#include "paths.h"
#include "settings.h"
#include "status.h"
#include "testhook.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef GF_SYSFS
#define GF_SYSFS "/sys/glowforge/"
#endif

#define POLL_US         50000       /* 20 steps a second: the hold steps at 2 Hz */
#define MEANS_EVERY_S   0.25
#define MEANS_FRESH_S   1.5
#define SETTING_EVERY_S 5.0
#define REFRESH_S       1.0         /* a heater above zero written again: the kernel's lease is 5 s */
#define CTX_STALE_S     3.0         /* an engine context older than this: the engine has the heater */
#define STAMP_FILE      "flow-heater-off"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;         /* ends the thread's long wait at a stop (flowrun_init makes it) */
static pthread_t th;
static int th_run;
static fw_t fw;
static flowrun_ctx_t ctx;
static int yielded;                 /* the engine has the heater */
static double yield_at = -1e9;
static double written;              /* what this file last wrote, percent */
static double written_at = -1e9;    /* when */
static char status_buf[512] = "{\"watch\":\"wait\"}";

static const char *sysfs_root(void)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    return r ? r : GF_SYSFS;
}

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A mean in counts as the engine reads a coolant channel (read_temp): the
 * air-assist shift taken off, then the per-machine correction. */
static double corrected_c(double counts, double offset_counts, double temp_offset_c)
{
    double c = counts - offset_counts;
    return coolant_degc_counts(c > 0.0 ? c : counts) + temp_offset_c;
}

/* The 1 s means of the two coolant channels, from pic/means, corrected as
 * the engine corrects its own readings. The file gives counts with three
 * decimals: a first line "hz H age_ms A reads R errors E", then a line a
 * channel, "name mean_1s mean_10s min_1s max_1s n_1s n_10s". A 1 s window
 * that holds fewer reads than half the sampler's rate is no reading: the
 * sampler starts its windows over after a broken run, and the mean of a
 * window's first reads is not a second's mean. */
static int means(double *down_c, double *up_c, double offset_counts, double temp_offset_c)
{
    char path[160], buf[1024];
    snprintf(path, sizeof(path), "%spic/means", sysfs_root());
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    double m1 = NAN, m2 = NAN, v, v10;
    unsigned hz = 0, rate, lo, hi, in_1s, n1 = 0, n2 = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (sscanf(line, "hz %u", &rate) == 1) {
            hz = rate;
        } else if (sscanf(line, "water_temp_1 %lf %lf %u %u %u", &v, &v10, &lo, &hi, &in_1s) == 5) {
            m1 = v;
            n1 = in_1s;
        } else if (sscanf(line, "water_temp_2 %lf %lf %u %u %u", &v, &v10, &lo, &hi, &in_1s) == 5) {
            m2 = v;
            n2 = in_1s;
        }
    }
    if (isnan(m1) || isnan(m2) || m1 <= 0.0 || m2 <= 0.0)
        return 0;
    if (!hz || 2 * n1 < hz || 2 * n2 < hz)
        return 0;
    *down_c = corrected_c(m1, offset_counts, temp_offset_c);
    *up_c = corrected_c(m2, offset_counts, temp_offset_c);
    return 1;
}

/* When this daemon's heater last went off, so a restart rests long enough
 * (the run directory clears at every boot, as the clock restarts). The
 * stamp stands only while the heater has been off since it: the watch's
 * own heat and the engine's taking of the heater both remove it. A start
 * that finds none takes the heater as just off and rests the full time: a
 * daemon killed in a hold wrote none, and the engine's heat (a warm-up) is
 * never stamped. */
static void stamp_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", ff_run_dir(), STAMP_FILE);
}

static void stamp_clear(void)
{
    char path[200];
    stamp_path(path, sizeof(path));
    unlink(path);
}

static void stamp_write(void)
{
    char path[200];
    stamp_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%.3f\n", mono_s());
        fclose(f);
    }
}

static double stamp_age(void)
{
    char path[200];
    stamp_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    double at = NAN;
    if (f) {
        if (fscanf(f, "%lf", &at) != 1)
            at = NAN;
        fclose(f);
    }
    double now = mono_s();
    return isnan(at) || at > now ? 0.0 : now - at;
}

static void heater_write(double pct)
{
    char path[160], val[24];
    if (pct < 0.0)
        pct = 0.0;
    snprintf(path, sizeof(path), "%sthermal/heater_pwm", sysfs_root());
    snprintf(val, sizeof(val), "%ld", lround(pct * 65535.0 / 100.0));
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, val, strlen(val)) < 0)
        fflog(LOG_ERR, "flow: the heater write failed: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    if (written > 0.0 && pct <= 0.0)
        stamp_write();
    else if (written <= 0.0 && pct > 0.0)
        stamp_clear();
    written = pct;
    written_at = mono_s();
}

/* A number from the settings, or the default when unset or not a number
 * (the range is the watch's to hold, flowcalc_cfg_clamp). */
static double setting_num(const char *key, double def)
{
    char v[32], *end;
    if (settings_get(key, v, sizeof(v)) != 0 || !v[0])
        return def;
    double d = strtod(v, &end);
    return end == v ? def : d;
}

static flowcalc_cfg_t read_cfg(void)
{
    flowcalc_cfg_t c;
    c.hold_c = setting_num("cool_flow_hold_c", FLOWCALC_HOLD_C_DEF);
    c.duty_min = setting_num("cool_flow_duty_min", FLOWCALC_DUTY_MIN_DEF);
    c.duty_max = setting_num("cool_flow_duty_max", FLOWCALC_DUTY_MAX_DEF);
    return flowcalc_cfg_clamp(c);
}

/* The loop's own state, kept between steps. */
static double next_means, next_setting;
static double m_at = -1e9, m_down = NAN, m_up = NAN;
static int enabled;
static flowcalc_cfg_t cfg;

/* One step at @now (the thread's: 20 a second with the watch on, one a
 * setting poll with it off): the setting, the readings, the watch, the
 * heater. With the setting off the sampler's file is not read. Returns 0
 * once the watch is told to stop. */
static int run_step(double now)
{
    fw_in_t in;
    memset(&in, 0, sizeof(in));
    in.now = now;
    double d, u, off_counts, off_c;
    pthread_mutex_lock(&mu);
    off_counts = ctx.offset_counts;
    off_c = ctx.temp_offset_c;
    pthread_mutex_unlock(&mu);
    if (now >= next_setting) {
        next_setting = now + SETTING_EVERY_S;
        enabled = settings_get_bool("cool_flow_watch", 0);
        cfg = read_cfg();
    }
    in.enabled = enabled;
    in.cfg = cfg;
    if (enabled && now >= next_means) {
        next_means = now + MEANS_EVERY_S;
        if (means(&d, &u, off_counts, off_c)) {
            m_at = now;
            m_down = d;
            m_up = u;
        }
    }
    if (now - m_at <= MEANS_FRESH_S) {
        in.have_means = 1;
        in.down_c = m_down;
        in.up_c = m_up;
    }

    pthread_mutex_lock(&mu);
    if (!th_run) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    int stale = now - ctx.t > CTX_STALE_S;
    in.yield = ctx.yield || yielded || stale;
    in.yield_why = ctx.yield ? ctx.yield_why
                 : stale ? "the cooling engine stopped ticking"
                 : "the engine took it";
    in.session = ctx.session;
    in.fans = ctx.fans;
    double want = fw_step(&fw, &in);
    /* A stale engine: the heater off here too (the engine is not there to
     * have done it), and nothing written after. */
    if (stale && !yielded && written != 0.0)
        heater_write(0.0);
    else if (!yielded && !stale &&
             (fabs(want - written) > 1e-6 || (want > 0.0 && now - written_at >= REFRESH_S)))
        heater_write(want);
    if (fw_status_json(&fw, status_buf, sizeof(status_buf)) < 0)
        snprintf(status_buf, sizeof(status_buf), "{\"watch\":\"%s\"}", fw_mode_name(fw.mode));
    pthread_mutex_unlock(&mu);
    return 1;
}

/* The wait between two steps: the hold's pace with the watch on; with it
 * off, the setting's poll, which a stop ends at once. */
static void pace(void)
{
    if (enabled) {
        usleep(POLL_US);
        return;
    }
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += (time_t)SETTING_EVERY_S;
    pthread_mutex_lock(&mu);
    while (th_run && pthread_cond_timedwait(&wake, &mu, &until) == 0)
        ;
    pthread_mutex_unlock(&mu);
}

static void *run_main(void *arg)
{
    (void)arg;
    while (run_step(mono_s()))
        pace();
    return NULL;
}

void flowrun_init(void)
{
    /* The stop's wake-up waits on the clock no step of the wall clock moves. */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&wake, &ca);
    pthread_condattr_destroy(&ca);
    pthread_mutex_lock(&mu);
    fw_init(&fw, fflog);
    ctx.fans = FW_FANS_IDLE;
    ctx.t = mono_s();               /* the engine's first tick is due within 1 s */
    cfg = flowcalc_cfg_default();
    fw_begin(&fw, mono_s(), stamp_age());
    th_run = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, NULL, run_main, NULL) != 0) {
        th_run = 0;
        fflog(LOG_ERR, "flow: the watch's thread failed to start; no flow watch");
    }
}

void flowrun_engine(const flowrun_ctx_t *c)
{
    pthread_mutex_lock(&mu);
    ctx = *c;
    if (!c->yield && c->t > yield_at)
        yielded = 0;
    pthread_mutex_unlock(&mu);
}

void flowrun_yield(const char *why)
{
    (void)why;
    pthread_mutex_lock(&mu);
    yielded = 1;
    yield_at = mono_s();
    if (written != 0.0)
        heater_write(0.0);
    /* What the engine does with the heater is not stamped. */
    stamp_clear();
    pthread_mutex_unlock(&mu);
}

void flowrun_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = th_run;
    th_run = 0;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(th, NULL);
    pthread_mutex_lock(&mu);
    if (written != 0.0)
        heater_write(0.0);
    pthread_mutex_unlock(&mu);
}

int flowrun_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    int n = snprintf(buf, len, "%s", status_buf);
    pthread_mutex_unlock(&mu);
    return n >= 0 && (size_t)n < len ? n : -1;
}
