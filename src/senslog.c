/*
 * senslog.c - forgectrl: the sensors logger (see senslog.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "senslog.h"

#include "airflow.h"
#include "cool.h"
#include "fflog.h"
#include "hooks.h"
#include "settings.h"
#include "setup.h"
#include "status.h"
#include "testhook.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define LOGGER          "sensors"
#define LINE_MAX_B      2000        /* under fflog's datagram, its header included */
#define INV_CHECK_S     60.0        /* how often the inventory and settings are compared */
#define INV_DAILY_S     86400.0
#define FR_MAX_PER_HOUR 6           /* a flapping warning cannot fill the logger */
#define DATA_DIR        "/data"

#ifndef GF_SYSFS
#define GF_SYSFS "/sys/glowforge/"
#endif

static const char *sysfs_root(void)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    return r ? r : GF_SYSFS;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th;
static int running = 0;
static int ch = -1;                 /* the fflog channel */
static senslog_ctx_t ctx;

/* Shared with senslog_event / senslog_flow (under mu). */
static senslog_jobs_t jobs;
static unsigned long ev_seq = 0;
static int fr_pending = 0;
static double fr_at = 0;
static char fr_msg[160];
static double fr_times[FR_MAX_PER_HOUR];
static unsigned fr_count = 0;

/* The thread's own. */
static senslog_ring_t ring;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double real_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void emit(const char *line, int n)
{
    if (n > 0)
        fflog_ch(ch, LOG_INFO, "%s", line);
}

/* --------------------------------------------------------------- reads */

static int rd_str(const char *attr, char *buf, size_t len)
{
    char path[160];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return 0;
}

static long rd_long(const char *attr)
{
    char buf[32];
    if (rd_str(attr, buf, sizeof(buf)) != 0)
        return -1;
    char *end;
    errno = 0;
    long v = strtol(buf, &end, 10);
    return (errno || end == buf || v < 0) ? -1 : v;
}

static int rd_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return n ? 0 : -1;
}

/* CPU busy percent over the interval since the counters were last
 * taken, from the /proc/stat aggregate. Its own counters: /status keeps
 * the panel's window apart. */
typedef struct {
    unsigned long long total, idle;
    int primed;
} cpu_ctr_t;

static double cpu_pct(cpu_ctr_t *c)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return NAN;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return NAN;
    unsigned long long total = 0;
    for (int i = 0; i < 8; i++)
        total += v[i];
    unsigned long long idle = v[3] + v[4];
    double pct = NAN;
    if (c->primed && total > c->total && idle >= c->idle) {
        unsigned long long dt = total - c->total, di = idle - c->idle;
        pct = di >= dt ? 0.0 : 100.0 * (double)(dt - di) / (double)dt;
    }
    c->total = total;
    c->idle = idle;
    c->primed = 1;
    return pct;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[96];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == ':') {
            v = strtol(line + kl + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

static void sample(senslog_sample_t *s, cpu_ctr_t *cpu)
{
    senslog_sample_clear(s);
    s->mono = mono_s();

    cool_view_t v;
    cool_view(&v);
    snprintf(s->phase, sizeof(s->phase), "%s", v.phase);
    snprintf(s->verdict, sizeof(s->verdict), "%s", v.verdict);
    s->hold = v.hold;
    s->fire_ok = v.fire_ok;
    s->armed = v.armed;

    if (rd_str("cnc/state", s->kst, sizeof(s->kst)) != 0)
        s->kst[0] = '\0';
    s->faults = rd_long("cnc/faults");
    unsigned long bits = machine_switch_bits();
    s->sw = (long)bits;

    s->down_raw = rd_long("pic/water_temp_1");
    s->up_raw = rd_long("pic/water_temp_2");
    if (s->down_raw >= 0)
        s->down_c = cool_coolant_c(s->down_raw);
    if (s->up_raw >= 0)
        s->up_c = cool_coolant_c(s->up_raw);
    double ch_c = chassis_degc(), soc = soc_degc();
    s->chassis_c = ch_c > -100 ? ch_c : NAN;
    s->soc_c = soc > -100 ? soc : NAN;
    s->psu_raw = rd_long("pic/pwr_temp");
    s->tec_raw = rd_long("pic/tec_temp");
    s->throttle = soc_throttle_state();

    s->exhaust_pwm = rd_long("thermal/exhaust_pwm");
    s->intake_pwm = rd_long("thermal/intake_pwm");
    s->aa_pwm = rd_long("head/air_assist_pwm");
    s->exhaust_ns = rd_long("thermal/tach_exhaust");
    s->intake1_ns = rd_long("thermal/tach_intake_1");
    s->intake2_ns = rd_long("thermal/tach_intake_2");
    s->aa_us = rd_long("head/air_assist_tach");
    /* The engine's own conversions: the exhaust and intakes count ns at
     * 2 pulses a revolution, the air assist us at 8. */
    if (s->exhaust_ns >= 0)
        s->exhaust_rpm = airflow_rpm(s->exhaust_ns, 1e9, 2);
    if (s->intake1_ns >= 0)
        s->intake1_rpm = airflow_rpm(s->intake1_ns, 1e9, 2);
    if (s->intake2_ns >= 0)
        s->intake2_rpm = airflow_rpm(s->intake2_ns, 1e9, 2);
    if (s->aa_us >= 0)
        s->aa_rpm = airflow_rpm(s->aa_us, 1e6, 8);
    s->purge = rd_long("head/purge_air");
    s->purge_cur = rd_long("head/purge_air_current");
    s->pump = rd_long("thermal/water_pump_on");
    s->heater_pwm = rd_long("thermal/heater_pwm");
    s->tec_on = rd_long("thermal/tec_on");

    s->hv = rd_long("pic/hv_current");
    s->hv_v = rd_long("pic/hv_voltage");
    s->lon = rd_long("cnc/laser_on_sampled");
    s->pgood = rd_long("cnc/laser_pgood_sampled");
    s->beam = rd_long("head/beam_detect_analog");
    s->beam_dig = rd_long("head/beam_detect_digital");
    s->beam_base = rd_long("head/beam_baseline");
    s->beam_n = rd_long("head/beam_samples");
    s->head_flags = rd_long("head/flags");
    for (int i = 0; i < 4; i++) {
        char attr[24];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i + 1);
        s->ir[i] = rd_long(attr);
    }
    s->hall = rd_long("head/hall_sensor");

    s->cpu_pct = cpu_pct(cpu);
    long total = meminfo_kb("MemTotal"), avail = meminfo_kb("MemAvailable");
    if (total > 0 && avail >= 0 && avail <= total)
        s->mem_pct = 100.0 * (double)(total - avail) / (double)total;
    s->tick_ms = cool_tick_max_ms();
    s->drops = fflog_dropped();
    struct statvfs sv;
    if (statvfs(DATA_DIR, &sv) == 0)
        s->data_free_mb = (long)((unsigned long long)sv.f_bavail * sv.f_frsize / (1024 * 1024));
}

/* ------------------------------------------------- inventory, settings */

/* key=value onto body, the value made one token. */
static void add_kv(char *body, size_t cap, const char *key, const char *val)
{
    size_t o = strlen(body);
    if (o + strlen(key) + 3 >= cap)
        return;
    int n = snprintf(body + o, cap - o, "%s%s=", o ? " " : "", key);
    if (n < 0)
        return;
    o += (size_t)n;
    const char *p = val && *val ? val : "-";
    for (; *p && o + 1 < cap; p++)
        body[o++] = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ? '_' : *p;
    body[o] = '\0';
}

static void add_kl(char *body, size_t cap, const char *key, long v)
{
    char b[24];
    if (v < 0)
        snprintf(b, sizeof(b), "-");
    else
        snprintf(b, sizeof(b), "%ld", v);
    add_kv(body, cap, key, b);
}

/* What the machine is. The SoC's unique id (soc0/serial_number) and the
 * head's serial number are identities and are never read here. */
static void inventory(char *body, size_t cap)
{
    char b[96];
    body[0] = '\0';
    read_fw_version(b, sizeof(b));
    add_kv(body, cap, "image", b);
    struct utsname u;
    add_kv(body, cap, "kernel", uname(&u) == 0 ? u.release : "-");
    add_kv(body, cap, "module", rd_file("/sys/module/glowforge/version", b, sizeof(b)) == 0 ? b : "-");
    add_kv(body, cap, "soc", rd_file("/sys/devices/soc0/soc_id", b, sizeof(b)) == 0 ? b : "-");
    add_kv(body, cap, "soc_rev", rd_file("/sys/devices/soc0/revision", b, sizeof(b)) == 0 ? b : "-");
    char trips[48] = "";
    for (int i = 0; i < 4; i++) {
        char path[96], t[16];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone0/trip_point_%d_temp", i);
        if (rd_file(path, t, sizeof(t)) != 0)
            break;
        size_t o = strlen(trips);
        snprintf(trips + o, sizeof(trips) - o, "%s%s", o ? "," : "", t);
    }
    add_kv(body, cap, "soc_trips", trips);
    add_kl(body, cap, "mem_kb", meminfo_kb("MemTotal"));
    /* The data partition's size; its free space moves with every write
     * and rides the beat instead, or every check would read as a change. */
    struct statvfs sv;
    if (statvfs(DATA_DIR, &sv) == 0)
        add_kl(body, cap, "data_kb", (long)((unsigned long long)sv.f_blocks * sv.f_frsize / 1024));
    add_kl(body, cap, "pic_id", rd_long("pic/id"));
    /* The head's info file: its id and firmware version. The serial line
     * is skipped, and so are the two register bytes: they move with the
     * lens and the Z current, and the beat carries the flag byte. */
    char info[256];
    char hid[16] = "", hver[16] = "";
    if (rd_str("head/info", info, sizeof(info)) == 0) {
        char *save = NULL;
        for (char *ln = strtok_r(info, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(ln, "id=", 3))
                snprintf(hid, sizeof(hid), "%s", ln + 3);
            else if (!strncmp(ln, "version=", 8))
                snprintf(hver, sizeof(hver), "%s", ln + 8);
        }
    }
    add_kv(body, cap, "head_id", hid);
    add_kv(body, cap, "head_fw", hver);
    /* The beam detector's coefficients, comma-separated hex. */
    char co[48] = "";
    if (rd_str("head/beam_coeffs", co, sizeof(co)) == 0)
        for (char *p = co; *p; p++)
            if (*p == ' ')
                *p = ',';
    add_kv(body, cap, "beam_coeffs", co);
    setup_machine_str("model", b, sizeof(b));
    add_kv(body, cap, "model", b);
    add_kl(body, cap, "tec", setup_machine_bool("tec"));
    setup_machine_str("camera", b, sizeof(b));
    add_kv(body, cap, "camera", b);
}

/* The settings in force: the cooling engine's resolved tunables, and the
 * named machine settings as stored ("-" for unset: the default stands).
 * A list, never the file: the file holds credentials. */
static const char *const cfg_keys[] = {
    "controller_mode", "homing_mode", "cloud_enabled", "ext_enabled",
    "xy_microsteps", "lid_policy", "lid_lamp_idle", "rail_settle_s",
    "laser_button_timeout_s", "laser_disarm_s", "laser_floor_density",
    "laser_dose_curve", "laser_corner_gamma", "laser_pulse_ticks",
    "laser_pulse_min_ticks", "lens_hall_edge_z_mm", "lens_park_z_mm",
    "lens_stop_below_steps", "lens_stop_above_steps", "tray_offset_mm",
    "cloud_pause_backtrack_ticks", "cloud_resume_lead_ticks", "cloud_hold_max_s",
};

static void settings_body(char *body, size_t cap)
{
    body[0] = '\0';
    cool_settings_in_force(body, cap);
    for (size_t i = 0; i < sizeof(cfg_keys) / sizeof(*cfg_keys); i++) {
        char v[128];
        if (settings_get(cfg_keys[i], v, sizeof(v)) != 0)
            v[0] = '\0';
        add_kv(body, cap, cfg_keys[i], v);
    }
}

/* -------------------------------------------------------------- thread */

static void boot_id(char *out, size_t len)
{
    char b[64];
    out[0] = '\0';
    if (rd_file("/proc/sys/kernel/random/boot_id", b, sizeof(b)) == 0)
        snprintf(out, len, "%.8s", b);
}

/* The verdicts whose arrival arms the flight recorder: every hold the
 * engine takes for a fault or a gate, not the warm-up (a cold room's
 * routine) nor an extension's hold. The engine's warnings that change no
 * verdict arm it themselves (senslog_event). */
static int fr_verdict(const char *v)
{
    static const char *const set[] = {
        "FIRE", "CRASH", "FLAME", "BUMP", "AIRFLOW", "CRITICAL", "SENSOR",
        "OVERTEMP", "COLD", "FAULT", "SUSPECT",
    };
    for (size_t i = 0; i < sizeof(set) / sizeof(*set); i++)
        if (!strcmp(v, set[i]))
            return 1;
    return 0;
}

static void fr_dump(unsigned long ev, double at, const char *msg)
{
    char line[LINE_MAX_B];
    double now = mono_s();
    emit(line, senslog_fmt_fr_head(line, sizeof(line), &ctx, now, ev, at, msg));
    for (int c = 0; c < senslog_fr_channels(); c++)
        emit(line, senslog_fmt_fr_channel(line, sizeof(line), &ctx, now, ev, &ring,
                                          at - SENSLOG_FR_PRE_S, at + SENSLOG_FR_POST_S, c));
}

static void *thread_main(void *arg)
{
    (void)arg;
    char line[LINE_MAX_B];
    char inv[1024], cfg[1600], inv_last[1024] = "", cfg_last[1600] = "";
    double inv_checked = -1e9, inv_emitted = -1e9;
    double last_beat = -1;
    char last_phase[12] = "", last_verdict[12] = "OK";
    double clock_off = real_s() - mono_s();
    double tick_acc = NAN;
    /* The first beat primes its counters and reports no load ("-"):
     * a window a few ms long says nothing. */
    cpu_ctr_t cpu_sec = {0}, cpu_beat = {0};

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (1) {
        pthread_mutex_lock(&mu);
        int run = running;
        pthread_mutex_unlock(&mu);
        if (!run)
            break;

        senslog_sample_t s;
        sample(&s, &cpu_sec);
        senslog_ring_push(&ring, &s);
        if (!isnan(s.tick_ms) && (isnan(tick_acc) || s.tick_ms > tick_acc))
            tick_acc = s.tick_ms;
        if (strcmp(s.verdict, last_verdict) && fr_verdict(s.verdict)) {
            cool_view_t v;
            cool_view(&v);
            char m[sizeof(fr_msg)];
            snprintf(m, sizeof(m), "verdict %s: %s", s.verdict, v.reason);
            senslog_event(m);
        }
        snprintf(last_verdict, sizeof(last_verdict), "%s", s.verdict);

        /* The job: fed the 1 Hz samples, written as it finishes. */
        pthread_mutex_lock(&mu);
        int finished = senslog_job_feed(&jobs, &s);
        senslog_job_t done = jobs.done;
        pthread_mutex_unlock(&mu);
        if (finished)
            emit(line, senslog_fmt_job(line, sizeof(line), &ctx, s.mono, &done));

        /* The beat carries the slowest engine tick and the CPU load since
         * the beat before it, not this second's alone. */
        if (senslog_beat_due(&s, last_beat, last_phase)) {
            senslog_sample_t b = s;
            b.tick_ms = tick_acc;
            b.cpu_pct = cpu_pct(&cpu_beat);
            emit(line, senslog_fmt_beat(line, sizeof(line), &ctx, &b));
            last_beat = s.mono;
            snprintf(last_phase, sizeof(last_phase), "%s", s.phase);
            tick_acc = NAN;
        }

        /* The clock. A step re-dates everything before it; the inventory
         * follows, so the first dated one is in the log. */
        double off = real_s() - s.mono;
        int stepped = senslog_clock_stepped(clock_off, off);
        if (stepped) {
            emit(line, senslog_fmt_clock(line, sizeof(line), &ctx, s.mono,
                                         clock_off + s.mono, off + s.mono));
            clock_off = off;
        }

        /* The inventory and the settings: at start, after a clock step, on
         * a change, and daily, so every bundle carries one however much
         * has rotated. */
        if (stepped || s.mono - inv_checked >= INV_CHECK_S) {
            inv_checked = s.mono;
            inventory(inv, sizeof(inv));
            settings_body(cfg, sizeof(cfg));
            const char *why = inv_last[0] == '\0' ? "start"
                            : stepped ? "clock"
                            : strcmp(inv, inv_last) || strcmp(cfg, cfg_last) ? "change"
                            : s.mono - inv_emitted >= INV_DAILY_S ? "daily" : NULL;
            if (why) {
                emit(line, senslog_fmt_kv(line, sizeof(line), "inv", &ctx, s.mono, why, inv));
                emit(line, senslog_fmt_kv(line, sizeof(line), "cfg", &ctx, s.mono, why, cfg));
                snprintf(inv_last, sizeof(inv_last), "%s", inv);
                snprintf(cfg_last, sizeof(cfg_last), "%s", cfg);
                inv_emitted = s.mono;
            }
        }

        /* The flight recorder, once its post-event window has passed. */
        pthread_mutex_lock(&mu);
        int dump = fr_pending && s.mono >= fr_at + SENSLOG_FR_POST_S;
        unsigned long ev = ev_seq;
        double at = fr_at;
        char msg[sizeof(fr_msg)];
        snprintf(msg, sizeof(msg), "%s", fr_msg);
        if (dump)
            fr_pending = 0;
        pthread_mutex_unlock(&mu);
        if (dump)
            fr_dump(ev, at, msg);

        next.tv_sec += 1;
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec > next.tv_sec ||
            (t1.tv_sec == next.tv_sec && t1.tv_nsec > next.tv_nsec))
            next = t1;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
    }
    return NULL;
}

/* ----------------------------------------------------------------- api */

void senslog_start(void)
{
    ch = fflog_channel(LOGGER);
    boot_id(ctx.boot, sizeof(ctx.boot));
    senslog_jobs_init(&jobs);
    pthread_mutex_lock(&mu);
    running = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, NULL, thread_main, NULL) != 0) {
        pthread_mutex_lock(&mu);
        running = 0;
        pthread_mutex_unlock(&mu);
        fflog(LOG_ERR, "senslog: the sensors logger's thread did not start");
        return;
    }
    fflog(LOG_INFO, "senslog: the sensors logger started");
}

void senslog_stop(void)
{
    pthread_mutex_lock(&mu);
    int was = running;
    running = 0;
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(th, NULL);
}

void senslog_event(const char *msg)
{
    double now = mono_s();
    pthread_mutex_lock(&mu);
    if (!fr_pending) {
        /* At most FR_MAX_PER_HOUR dumps in any hour. */
        unsigned recent = 0;
        for (unsigned i = 0; i < fr_count && i < FR_MAX_PER_HOUR; i++)
            if (now - fr_times[i] < 3600.0)
                recent++;
        if (recent < FR_MAX_PER_HOUR) {
            fr_times[fr_count % FR_MAX_PER_HOUR] = now;
            fr_count++;
            ev_seq++;
            fr_pending = 1;
            fr_at = now;
            snprintf(fr_msg, sizeof(fr_msg), "%s", msg ? msg : "");
        }
    }
    pthread_mutex_unlock(&mu);
}

void senslog_flow(const senslog_flow_t *f)
{
    char line[LINE_MAX_B];
    emit(line, senslog_fmt_flow(line, sizeof(line), &ctx, mono_s(), f));
    if (f->src && !strcmp(f->src, "check")) {
        pthread_mutex_lock(&mu);
        senslog_job_flow(&jobs, f->rise);
        pthread_mutex_unlock(&mu);
    }
}
