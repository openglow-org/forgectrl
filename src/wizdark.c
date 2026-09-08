/*
 * wizdark.c - the dark wizards: the machine checked with the laser locked
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * One worker thread runs one wizard. The page polls the status and
 * answers prompts; the worker blocks on the answer. Every wizard
 * restores what it took on every exit path: the controller through the
 * supervisor, the thermal hardware through the cooling engine's takeover
 * helpers, the crash watch and the loopback posture through their own
 * calls. The decisions live in wizcalc.c; this file is the hardware.
 */
#define _GNU_SOURCE
#include "wizdark.h"
#include "accel.h"
#include "airflow.h"
#include "cam.h"
#include "commission.h"
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "gates.h"
#include "hooks.h"
#include "led.h"
#include "liveness.h"
#include "paths.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "wizcalc.h"
#include "wizrun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PROMPT_TIMEOUT_S 600     /* an eye at a sheet takes its time */
#define LOG_LINES 32
#define LOG_LEN   112
/* The jog witness: the head accelerometer over the crash watch's own
 * i2c path, at the armed window's 800 Hz and +/-4 g, read at about 175
 * samples a second (the sysfs one-shots are too slow for a one-second
 * jog, and each one powers the part down under the crash watch). The
 * signature judged is the peak-to-peak of the signal averaged over the
 * last JOG_LP_N samples (about 30 ms): the wideband vibration averages
 * out, and what stands is the commanded acceleration itself, a step of
 * about 500 counts up at the start of the jog and the same down at its
 * end (700 and 590 mm/s^2 on X and Y at 8192 counts per g). Bench
 * numbers: at rest with the pump and fans on, 150 to 320 counts on each
 * axis; a 50 mm jog at 3000 mm/min reads 1300 to 3000 on X and 800 to
 * 1400 on Y. Moving is the busier axis at 2.5 times its rest reading
 * and 400 counts or more. The RMS on each axis is recorded beside it.
 * Each window runs until the kernel has played the whole move: the
 * driver reports Idle when the stream is produced, up to half a second
 * before the pulse engine finishes it. */
#define JOG_LP_N      5
#define JOG_LP_RATIO  2.5
#define JOG_LP_FLOOR  400.0
#define JOG_REST_S    1.0
#define JOG_DRAIN_S   3.0

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static struct {
    char id[32];
    int running;
    volatile int abort_req;
    char phase[96];
    int progress;
    time_t started;
    char log[LOG_LINES][LOG_LEN];
    int log_n;
    /* the open prompt, and the answer to it */
    int prompt_seq;                 /* 0 = none open */
    char prompt_id[24], prompt_kind[12], prompt_text[320];
    char prompt_opt[WIZ_PROMPT_OPTS][24];
    int prompt_nopt;
    time_t prompt_opened;           /* when the open prompt went up */
    int prompt_timeout;             /* and how long it waits */
    int answer_seq;
    char answer[64];
    /* the outcome */
    json_t *result, *applied;
    char error[192];
    /* the login session driving the run ("" for a tool); a second
     * browser mirrors until it takes the run over */
    char owner[SESSION_ID_HEX + 1];
} S;
static int seq_counter;
/* The wizard holds the machine with no controller running: the button
 * breathes white until the controller is handed the LED back. */
static int machine_held;

static uint8_t *shot_jpeg[2];
static size_t shot_len[2];

/* ------------------------------------------------------------- sysfs */

static const char *sysfs_root(void)
{
    const char *r = getenv("GF_SYSFS_ROOT");
    return r && *r ? r : "/sys/glowforge";
}

static long rd_long(const char *attr, long fallback)
{
    char path[192], text[32];
    snprintf(path, sizeof(path), "%s/%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fallback;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return fallback;
    text[n] = '\0';
    return strtol(text, NULL, 10);
}

static int wr_attr(const char *attr, const char *val)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", sysfs_root(), attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------- progress and log */

static void wlog(const char *fmt, ...)
{
    char line[92];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    long el = (long)(time(NULL) - S.started);
    if (S.log_n == LOG_LINES) {
        memmove(S.log[0], S.log[1], sizeof(S.log[0]) * (LOG_LINES - 1));
        S.log_n--;
    }
    snprintf(S.log[S.log_n++], LOG_LEN, "%02ld:%02ld %.88s", (el / 60) % 100, el % 60, line);
    pthread_mutex_unlock(&mu);
    fflog(LOG_INFO, "wiz %s: %s", S.id, line);
}

static void phase(const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    snprintf(S.phase, sizeof(S.phase), "%s", text);
    pthread_mutex_unlock(&mu);
}

static void progress(int pct)
{
    pthread_mutex_lock(&mu);
    S.progress = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    pthread_mutex_unlock(&mu);
}

static int aborted(void)
{
    pthread_mutex_lock(&mu);
    int a = S.abort_req;
    pthread_mutex_unlock(&mu);
    return a;
}

/* Wait up to timeout_s for cond(ctx) to be true, polling at 100 ms;
 * -1 on abort, -2 on timeout, 0 when the condition held. */
static int wait_for(int (*cond)(void *), void *ctx, int timeout_s)
{
    double deadline = wall_s() + timeout_s;
    for (;;) {
        if (aborted())
            return -1;
        if (cond(ctx))
            return 0;
        if (wall_s() > deadline)
            return -2;
        msleep(100);
    }
}

/* Put a prompt up and block for the answer. kind: "continue" (one
 * button), "confirm" (yes or no), "number" (a value, or skip), "choice"
 * (the options). 0 with the answer text, -1 aborted, -2 timed out
 * after timeout_s. The page shows how long the prompt waits. */
static int ask_t(const char *kind, const char *pid, const char *text,
                 const char *const *opts, int nopt, int timeout_s, char *answer, size_t alen)
{
    pthread_mutex_lock(&mu);
    int seq = ++seq_counter;
    S.prompt_seq = seq;
    snprintf(S.prompt_id, sizeof(S.prompt_id), "%s", pid);
    snprintf(S.prompt_kind, sizeof(S.prompt_kind), "%s", kind);
    snprintf(S.prompt_text, sizeof(S.prompt_text), "%s", text);
    S.prompt_nopt = 0;
    for (int i = 0; i < nopt && i < WIZ_PROMPT_OPTS; i++)
        snprintf(S.prompt_opt[S.prompt_nopt++], sizeof(S.prompt_opt[0]), "%s", opts[i]);
    S.prompt_opened = time(NULL);
    S.prompt_timeout = timeout_s;
    S.answer_seq = 0;
    S.answer[0] = '\0';
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_s;
    int rc = 0;
    while (S.answer_seq != seq && !S.abort_req) {
        if (pthread_cond_timedwait(&cv, &mu, &until) == ETIMEDOUT) {
            rc = -2;
            break;
        }
    }
    if (S.abort_req)
        rc = -1;
    if (rc == 0 && alen)
        snprintf(answer, alen, "%s", S.answer);
    S.prompt_seq = 0;
    pthread_mutex_unlock(&mu);
    if (rc == -2)
        wlog("no answer to \"%s\" within %d s", pid, timeout_s);
    return rc;
}

static int ask(const char *kind, const char *pid, const char *text,
               const char *const *opts, int nopt, char *answer, size_t alen)
{
    return ask_t(kind, pid, text, opts, nopt, PROMPT_TIMEOUT_S, answer, alen);
}

/* A "wait" prompt: the page shows the text and the phase while the
 * machine itself answers (a switch edge, the press a job waits for).
 * wait_open puts it up; wait_close takes it down. */
static void wait_open(const char *pid, const char *text, int timeout_s)
{
    pthread_mutex_lock(&mu);
    S.prompt_seq = ++seq_counter;
    snprintf(S.prompt_id, sizeof(S.prompt_id), "%s", pid);
    snprintf(S.prompt_kind, sizeof(S.prompt_kind), "wait");
    snprintf(S.prompt_text, sizeof(S.prompt_text), "%s", text);
    S.prompt_nopt = 0;
    S.prompt_opened = time(NULL);
    S.prompt_timeout = timeout_s;
    pthread_mutex_unlock(&mu);
}

static void wait_close(void)
{
    pthread_mutex_lock(&mu);
    S.prompt_seq = 0;
    pthread_mutex_unlock(&mu);
}

static int ask_continue(const char *pid, const char *text)
{
    static const char *const o[] = { "Continue" };
    char a[8];
    return ask("continue", pid, text, o, 1, a, sizeof(a));
}

/* 1 yes, 0 no, -1 aborted or timed out. */
static int ask_confirm(const char *pid, const char *text)
{
    static const char *const o[] = { "Yes", "No" };
    char a[8];
    int rc = ask("confirm", pid, text, o, 2, a, sizeof(a));
    if (rc != 0)
        return -1;
    return !strcasecmp(a, "yes") || !strcmp(a, "1");
}

/* ------------------------------------------------------------ finish */

static void finish_err(const char *fmt, ...)
{
    char text[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    snprintf(S.error, sizeof(S.error), "%s", text);
    pthread_mutex_unlock(&mu);
    fflog(LOG_WARNING, "wiz %s: %s", S.id, text);
}

/* The wizard completed. finish_ok borrows result and applied: the caller
 * still owns them and decrefs them. commission_wizard_done consumes what
 * it is handed, so it gets its own references; a NULL applied means the
 * empty object. */
static void finish_ok(int version, json_t *result, json_t *applied)
{
    pthread_mutex_lock(&mu);
    if (S.result)
        json_decref(S.result);
    S.result = json_incref(result);
    if (S.applied)
        json_decref(S.applied);
    S.applied = applied ? json_incref(applied) : NULL;
    pthread_mutex_unlock(&mu);
    if (commission_wizard_done(S.id, version, json_incref(result),
                               applied ? json_incref(applied) : json_object()) != 0)
        finish_err("the record could not be written");
    else
        fflog(LOG_NOTICE, "wiz %s: complete", S.id);
}

/* One validated multi-key settings write, the before values recorded. */
static int write_settings(const char *const *keys, const char *const *vals, size_t n,
                          json_t *applied, char *err, size_t elen)
{
    const char *k[16], *v[16];
    size_t m = 0;
    for (size_t i = 0; i < n && m < 16; i++) {
        if (!vals[i])
            continue;
        if (!setting_valid(keys[i], vals[i])) {
            snprintf(err, elen, "invalid value for %s: %s", keys[i], vals[i]);
            return -1;
        }
        char from[192] = "";
        settings_get(keys[i], from, sizeof(from));
        json_t *e = json_object();
        json_object_set_new(e, "from", json_string(from));
        json_object_set_new(e, "to", json_string(vals[i]));
        json_object_set_new(applied, keys[i], e);
        k[m] = keys[i];
        v[m] = vals[i];
        m++;
    }
    if (m && settings_set_many(k, v, m) != 0) {
        snprintf(err, elen, "cannot write the settings");
        return -1;
    }
    return 0;
}

static double setting_num(const char *key, double def)
{
    char v[64];
    if (settings_get(key, v, sizeof(v)) == 0 && v[0])
        return strtod(v, NULL);
    return def;
}

/* The compiled default of a gate setting, or the value in force. */
static double gate_value(const char *key)
{
    const gate_setting_t *g = gate_setting_find(key);
    return setting_num(key, g ? g->def : 0.0);
}

/* ------------------------------------------------------- the switches */

struct bitwait { unsigned bit; int want; };

static int bit_is(void *ctx)
{
    struct bitwait *w = ctx;
    unsigned long bits = machine_switch_bits();
    return ((bits >> w->bit) & 1u) == (unsigned)w->want;
}

/* Ask for an edge on `bit` toward `want`; returns 0 when seen. */
static int edge(const char *pid, const char *text, unsigned bit, int want, int timeout_s)
{
    struct bitwait w = { bit, want };
    /* A wait for the lid to close is the machine asking for attention:
     * the button blinks amber until the lid answers. */
    int lid = bit == 3 && want == 1;
    phase("%s", text);
    wait_open(pid, text, timeout_s);
    if (lid)
        wiz_led_attention(1);
    int rc = wait_for(bit_is, &w, timeout_s);
    if (lid)
        wiz_led_attention(0);
    wait_close();
    if (rc == 0)
        wlog("%s: seen", pid);
    return rc;
}

/* --------------------------------------- the controller and the LED */

int wiz_controller_stop(void)
{
    int rc = super_controller_stop();
    if (rc == 0) {
        machine_held = 1;
        led_set(LED_BREATHE_WHITE);
    }
    return rc;
}

void wiz_controller_start(void)
{
    machine_held = 0;
    led_release();
    super_controller_start();
}

void wiz_led_attention(int on)
{
    if (on)
        led_set(LED_BLINK_AMBER);
    else if (machine_held)
        led_set(LED_BREATHE_WHITE);
    else
        led_release();
}

static void run_switches(void)
{
    /* EV_SW bits: 2 button, 3 doors (both lid switches, closed = 1),
     * 4 hv_enable readback, 5 interlock (active = the loop is open),
     * 7 head (status.c). */
    json_t *r = json_object();
    char model[24] = "";
    commission_machine_str("model", model, sizeof(model));
    int pro = !strcmp(model, "pro");
    unsigned long bits = machine_switch_bits();
    wlog("switches at start: 0x%02lx", bits);
    progress(5);

    if (!((bits >> 3) & 1u)) {
        if (edge("lid-close-first", "Close the lid to begin.", 3, 1, PROMPT_TIMEOUT_S) != 0)
            goto fail;
    }
    int hv_closed = (int)((machine_switch_bits() >> 4) & 1u);
    if (edge("lid-open", "Open the lid.", 3, 0, PROMPT_TIMEOUT_S) != 0)
        goto fail;
    msleep(300);
    int hv_open = (int)((machine_switch_bits() >> 4) & 1u);
    progress(25);
    if (edge("lid-close", "Close the lid.", 3, 1, PROMPT_TIMEOUT_S) != 0)
        goto fail;
    msleep(300);
    int hv_closed2 = (int)((machine_switch_bits() >> 4) & 1u);
    json_object_set_new(r, "lid", json_true());
    /* HV enable follows the lid while the safing chain is fed; a machine
     * whose readback does not move is recorded, not failed: the chain
     * needs the charge pump, which no controller feeds during this step. */
    int hv_follows = hv_closed && !hv_open && hv_closed2;
    json_object_set_new(r, "hv_enable_follows_lid", json_boolean(hv_follows));
    wlog("HV enable readback: closed %d, open %d, closed %d%s", hv_closed, hv_open, hv_closed2,
         hv_follows ? "" : " (did not follow the lid)");
    progress(45);

    if (edge("button-press", "Press the button once.", 2, 1, PROMPT_TIMEOUT_S) != 0)
        goto fail;
    if (edge("button-release", "Release the button.", 2, 0, 30) != 0)
        goto fail;
    json_object_set_new(r, "button", json_true());
    progress(65);

    if (pro) {
        if (edge("interlock-open", "Open the interlock loop (unplug the interlock connector).",
                 5, 1, PROMPT_TIMEOUT_S) != 0)
            goto fail;
        if (edge("interlock-close", "Close the interlock loop again.", 5, 0, PROMPT_TIMEOUT_S) != 0)
            goto fail;
        json_object_set_new(r, "interlock", json_string("tested"));
    } else {
        int satisfied = !((machine_switch_bits() >> 5) & 1u);
        json_object_set_new(r, "interlock", json_string(satisfied ? "satisfied" : "open"));
        if (!satisfied) {
            finish_err("the interlock loop reads open on a machine without a connector");
            json_decref(r);
            return;
        }
    }
    progress(85);

    int head = rd_long("head/hall_sensor", -1) >= 0;
    json_object_set_new(r, "head_present", json_boolean(head));
    wlog("head: %s", head ? "present" : "not readable");
    if (!head) {
        finish_err("the head does not answer (hall sensor unreadable): seat the head and run again");
        json_decref(r);
        return;
    }
    progress(100);
    finish_ok(1, r, NULL);
    json_decref(r);
    return;
fail:
    if (!aborted())
        finish_err("the switch did not change within the time allowed");
    json_decref(r);
}

/* -------------------------------------------------------- the sensors */

static void run_sensors(void)
{
    const int secs = 10, hz = 2;
    int n = secs * hz;
    double down = 0, up = 0, chassis = 0, soc = 0;
    long ir_max[4] = { -1, -1, -1, -1 };
    long hv_max = 0;
    double ex_sum = 0, in_sum = 0;
    int accel_events = 0, have_temp = 0;
    int crash = crash_hw_open() == 0;
    crash_watch_t cw;
    crash_reset(&cw);
    if (crash)
        crash_hw_arm((int)gate_value("cool_accel_x_alert"), (int)gate_value("cool_accel_y_alert"),
                     (int)gate_value("cool_accel_abort"));
    phase("sampling the sensors at rest for %d s", secs);
    for (int i = 0; i < n; i++) {
        if (aborted())
            goto out;
        long t1 = rd_long("pic/water_temp_1", -1), t2 = rd_long("pic/water_temp_2", -1);
        if (t1 > 0 && t2 > 0) {
            down += coolant_degc(t1);
            up += coolant_degc(t2);
            have_temp++;
        }
        for (int k = 0; k < 4; k++) {
            char attr[20];
            snprintf(attr, sizeof(attr), "pic/lid_ir_%d", k + 1);
            long v = rd_long(attr, -1);
            if (v > ir_max[k])
                ir_max[k] = v;
        }
        long hv = rd_long("pic/hv_current", 0);
        if (hv > hv_max)
            hv_max = hv;
        ex_sum += airflow_rpm(rd_long("thermal/tach_exhaust", 0), 1e9, 2);
        in_sum += airflow_rpm(rd_long("thermal/tach_intake_1", 0), 1e9, 2);
        if (crash) {
            /* The engine's interpreter decides what a source register
             * means; the raw bits alone read as motion on a still head. */
            unsigned s1 = 0, s2 = 0;
            if (crash_hw_poll(&s1, &s2) == 0) {
                crash_event_t e = crash_tick(&cw, s1, s2);
                if (e == CrashEv_Alert || e == CrashEv_Alarm)
                    accel_events++;
            }
        }
        progress(5 + i * 80 / n);
        msleep(1000 / hz);
    }
    chassis = chassis_degc();
    soc = soc_degc();
    wizcalc_sensors_t s = {
        .down_c = have_temp ? down / have_temp : NAN,
        .up_c = have_temp ? up / have_temp : NAN,
        .chassis_c = chassis > -50.0 ? chassis : NAN,
        .soc_c = soc > -50.0 ? soc : NAN,
        .ir = { ir_max[0], ir_max[1], ir_max[2], ir_max[3] },
        .ir_alert = fmin(gate_value("cool_fire_q1_alert"), gate_value("cool_fire_q2_alert")),
        .accel_events = crash ? accel_events : 0,
        .pgood = rd_long("cnc/laser_pgood_sampled", -1),
        .hv_current = hv_max,
        /* The fans are off at idle (duty 0), so the tachs are reported,
         * not judged; the airflow check proves them at the run profile. */
        .tach_exhaust = -1.0,
        .tach_intake = -1.0,
    };
    char why[256];
    int bad = wizcalc_sensors_judge(&s, why, sizeof(why));
    json_t *r = json_object();
    json_object_set_new(r, "coolant_down_c", json_real(round(s.down_c * 10) / 10));
    json_object_set_new(r, "coolant_up_c", json_real(round(s.up_c * 10) / 10));
    json_object_set_new(r, "chassis_c", isnan(s.chassis_c) ? json_null() : json_real(round(s.chassis_c * 10) / 10));
    json_object_set_new(r, "soc_c", isnan(s.soc_c) ? json_null() : json_real(round(s.soc_c * 10) / 10));
    json_t *ir = json_array();
    for (int k = 0; k < 4; k++)
        json_array_append_new(ir, json_integer(ir_max[k]));
    json_object_set_new(r, "lid_ir_max", ir);
    json_object_set_new(r, "accel_events", json_integer(s.accel_events));
    json_object_set_new(r, "accel_watched", json_boolean(crash));
    json_object_set_new(r, "laser_pgood", json_integer(s.pgood > 0 ? 1 : 0));
    json_object_set_new(r, "hv_current_max", json_integer(hv_max));
    json_object_set_new(r, "exhaust_rpm_idle", json_real(round(ex_sum / n)));
    json_object_set_new(r, "intake_rpm_idle", json_real(round(in_sum / n)));
    json_object_set_new(r, "purge_current", json_integer(rd_long("head/purge_air_current", -1)));
    wlog("coolant %.1f/%.1f C, chassis %.1f, SoC %.1f, IR max %ld/%ld/%ld/%ld, pgood %ld, HV %ld",
         s.down_c, s.up_c, s.chassis_c, s.soc_c, ir_max[0], ir_max[1], ir_max[2], ir_max[3],
         s.pgood, hv_max);
    if (bad) {
        finish_err("%s", why);
        json_decref(r);
        goto out;
    }
    progress(90);
    /* The optional room temperature: one point that gives the coolant
     * sensors a per-machine offset. */
    json_t *applied = json_object();
    static const char *const o[] = { "Set", "Skip" };
    char a[32];
    int rc = ask("number", "room-temp",
                 "Optional: if you have a thermometer, enter the room temperature in C and the "
                 "coolant readings get a per-machine offset. Otherwise skip.", o, 2, a, sizeof(a));
    if (rc == -1) {
        json_decref(r);
        json_decref(applied);
        goto out;
    }
    if (rc == 0 && a[0] && strcasecmp(a, "skip")) {
        double room = strtod(a, NULL);
        double mean = (s.down_c + s.up_c) / 2.0;
        double offset = room - mean;
        char val[24];
        snprintf(val, sizeof(val), "%.1f", offset);
        const char *keys[] = { "cool_temp_offset_c" };
        const char *vals[] = { val };
        char err[96];
        if (fabs(offset) > 5.0)
            wlog("room %.1f C is %.1f C from the coolant reading: no offset written (limit 5)", room, offset);
        else if (write_settings(keys, vals, 1, applied, err, sizeof(err)) == 0) {
            json_object_set_new(r, "temp_offset_c", json_real(round(offset * 10) / 10));
            wlog("coolant offset %.1f C from a room reading of %.1f C", offset, room);
        } else
            wlog("offset not written: %s", err);
    }
    progress(100);
    finish_ok(1, r, applied);
    json_decref(r);
    json_decref(applied);
out:
    if (crash) {
        crash_hw_disarm();
        crash_hw_close();
    }
}

/* -------------------------------------------------------- the airflow */

enum { F_EXHAUST, F_INTAKE1, F_INTAKE2, F_AIR, F_N };
static const char *const fan_names[F_N] = { "exhaust", "intake 1", "intake 2", "air assist" };

static void run_airflow(void)
{
    const double hz = 8.0;
    const int secs = 35;
    int n = (int)(hz * secs);
    double *trace = calloc((size_t)n * F_N, sizeof(double));
    double *purge = calloc((size_t)n, sizeof(double));
    int took = 0, stopped = 0;
    json_t *r = NULL, *applied = NULL;
    if (!trace || !purge) {
        finish_err("out of memory");
        goto out;
    }
    phase("stopping the motion controller");
    if (wiz_controller_stop() != 0) {
        finish_err("could not stop the motion controller");
        goto out;
    }
    stopped = 1;
    if (cool_diag_take() != 0) {
        finish_err("the cooling engine is busy (a session, its cooldown, or another takeover)");
        goto out;
    }
    took = 1;
    phase("fans to the run profile for %d s", secs);
    wlog("fans to run duty, purge on");
    cool_diag_fans_run();
    cool_diag_purge(1);
    double t0 = wall_s();
    for (int i = 0; i < n; i++) {
        if (aborted())
            goto out;
        trace[i * F_N + F_EXHAUST] = airflow_rpm(rd_long("thermal/tach_exhaust", 0), 1e9, 2);
        trace[i * F_N + F_INTAKE1] = airflow_rpm(rd_long("thermal/tach_intake_1", 0), 1e9, 2);
        trace[i * F_N + F_INTAKE2] = airflow_rpm(rd_long("thermal/tach_intake_2", 0), 1e9, 2);
        trace[i * F_N + F_AIR] = airflow_rpm(rd_long("head/air_assist_tach", 0), 1e6, 8);
        purge[i] = (double)rd_long("head/purge_air_current", 0);
        progress(5 + i * 75 / n);
        double next = t0 + (i + 1) / hz;
        double now = wall_s();
        if (next > now)
            msleep((int)((next - now) * 1000.0));
    }
    phase("purge off, reading the off current");
    cool_diag_purge(0);
    msleep(3000);
    double purge_off = (double)rd_long("head/purge_air_current", 0);
    /* Judge each fan. */
    wizcalc_fan_t f[F_N];
    double col[400];
    int fail = 0;
    double spin_max = 0.0;
    r = json_object();
    json_t *fans = json_object();
    for (int k = 0; k < F_N; k++) {
        for (int i = 0; i < n && i < 400; i++)
            col[i] = trace[i * F_N + k];
        wizcalc_fan_analyze(col, n < 400 ? n : 400, hz, 1000.0, &f[k]);
        json_t *o = json_object();
        json_object_set_new(o, "steady_rpm", json_real(round(f[k].steady_rpm)));
        json_object_set_new(o, "spinup_s", json_real(round(f[k].spinup_s * 10) / 10));
        json_object_set_new(o, "ok", json_boolean(f[k].ok));
        json_object_set_new(fans, fan_names[k], o);
        wlog("%s: %.0f rpm steady, 90 percent at %.1f s%s", fan_names[k], f[k].steady_rpm,
             f[k].spinup_s, f[k].ok ? "" : " - UNDER 1000 RPM");
        if (!f[k].ok)
            fail = 1;
        if (f[k].spinup_s > spin_max)
            spin_max = f[k].spinup_s;
    }
    json_object_set_new(r, "fans", fans);
    wizcalc_fan_t p;
    double purge_on = 0.0;
    if (wizcalc_fan_analyze(purge, n < 400 ? n : 400, hz, 0.0, &p) == 0)
        purge_on = p.steady_rpm;
    json_object_set_new(r, "purge_current_on", json_real(round(purge_on)));
    json_object_set_new(r, "purge_current_off", json_real(round(purge_off)));
    wlog("purge current %.0f on, %.0f off", purge_on, purge_off);
    if (purge_on < purge_off + 100.0) {
        wlog("the purge fan draws no more current on than off");
        fail = 1;
    }
    if (fail) {
        finish_err("a fan did not come up to speed at the run profile: see the log");
        goto out;
    }
    /* The floors and the grace, inside each setting's legal range. */
    const gate_setting_t *ge = gate_setting_find("cool_tach_exhaust_min_rpm");
    const gate_setting_t *gi = gate_setting_find("cool_tach_intake_min_rpm");
    const gate_setting_t *ga = gate_setting_find("cool_tach_air_assist_min_rpm");
    const gate_setting_t *gp = gate_setting_find("cool_purge_min_current");
    const gate_setting_t *gg = gate_setting_find("cool_fan_grace_s");
    double intake_steady = fmin(f[F_INTAKE1].steady_rpm, f[F_INTAKE2].steady_rpm);
    double fl_ex = wizcalc_floor(f[F_EXHAUST].steady_rpm, 0.55, ge ? ge->band_lo : 0, ge ? ge->band_hi : 1e9);
    double fl_in = wizcalc_floor(intake_steady, 0.55, gi ? gi->band_lo : 0, gi ? gi->band_hi : 1e9);
    double fl_aa = wizcalc_floor(f[F_AIR].steady_rpm, 0.55, ga ? ga->band_lo : 0, ga ? ga->band_hi : 1e9);
    double fl_pg = wizcalc_floor(purge_on, 0.55, gp ? gp->band_lo : 0, gp ? gp->band_hi : 1e9);
    double grace = wizcalc_grace_s(spin_max, gg ? gg->band_lo : 5, gg ? gg->band_hi : 60);
    char v_ex[24], v_in[24], v_aa[24], v_pg[24], v_gr[24];
    snprintf(v_ex, sizeof(v_ex), "%.0f", fl_ex);
    snprintf(v_in, sizeof(v_in), "%.0f", fl_in);
    snprintf(v_aa, sizeof(v_aa), "%.0f", fl_aa);
    snprintf(v_pg, sizeof(v_pg), "%.0f", fl_pg);
    snprintf(v_gr, sizeof(v_gr), "%.0f", grace);
    const char *keys[] = { "cool_tach_exhaust_min_rpm", "cool_tach_intake_min_rpm",
                           "cool_tach_air_assist_min_rpm", "cool_purge_min_current",
                           "cool_fan_grace_s" };
    const char *vals[] = { v_ex, v_in, v_aa, v_pg, v_gr };
    json_t *floors = json_object();
    for (int i = 0; i < 5; i++)
        json_object_set_new(floors, keys[i], json_string(vals[i]));
    json_object_set_new(r, "floors", floors);
    wlog("floors: exhaust %s, intake %s, air assist %s, purge %s; grace %s s", v_ex, v_in, v_aa,
         v_pg, v_gr);
    phase("standing the fans down");
    cool_diag_fans_idle();
    cool_diag_release();
    took = 0;
    wiz_controller_start();
    stopped = 0;
    applied = json_object();
    char err[96];
    if (write_settings(keys, vals, 5, applied, err, sizeof(err)) != 0) {
        finish_err("%s", err);
        goto out;
    }
    progress(100);
    finish_ok(1, r, applied);
out:
    if (took) {
        cool_diag_purge(0);
        cool_diag_fans_idle();
        cool_diag_release();
    }
    if (stopped)
        wiz_controller_start();
    if (r)
        json_decref(r);
    if (applied)
        json_decref(applied);
    free(trace);
    free(purge);
}

/* ------------------------------------------- the cooling diagnostics */

/* Run one of diag.c's tools to its end; the parsed result object, or
 * NULL after an abort. expect_s is the typical run time for the
 * progress estimate. */
static json_t *run_diag(const char *tool, int expect_s, int pct_from, int pct_to)
{
    int rc = diag_start(tool);
    if (rc != 0) {
        finish_err(rc == -2 ? "the machine is not idle, or the cooling engine is busy"
                            : "the diagnostic could not start");
        return NULL;
    }
    char buf[2048];
    double t0 = wall_s();
    for (;;) {
        msleep(1000);
        if (aborted())
            diag_abort();
        if (diag_status_json(buf, sizeof(buf)) < 0)
            continue;
        json_t *st = json_loads(buf, 0, NULL);
        if (!st)
            continue;
        const char *ph = json_string_value(json_object_get(st, "phase"));
        if (ph && *ph)
            phase("%s", ph);
        double el = wall_s() - t0;
        progress(pct_from + (int)((pct_to - pct_from) * fmin(el / expect_s, 0.98)));
        json_t *res = json_object_get(st, "result");
        int running = json_is_true(json_object_get(st, "running"));
        if (!running) {
            json_t *out = json_is_object(res) ? json_incref(res) : NULL;
            json_decref(st);
            if (!out) {
                if (!aborted())
                    finish_err("the diagnostic ended without a result");
                return NULL;
            }
            return out;
        }
        json_decref(st);
    }
}

static void run_cooling_aa(void)
{
    phase("the air-assist offset: settling the loop");
    json_t *res = run_diag("aa-offset-calibrate", 360, 2, 95);
    if (!res)
        return;
    const char *err = json_string_value(json_object_get(res, "error"));
    if (err) {
        finish_err("%s", err);
        json_decref(res);
        return;
    }
    double rec = json_number_value(json_object_get(res, "recommend"));
    char val[24];
    snprintf(val, sizeof(val), "%.0f", rec);
    const char *keys[] = { "cool_aa_offset_counts" };
    const char *vals[] = { val };
    json_t *applied = json_object();
    char e[96];
    if (write_settings(keys, vals, 1, applied, e, sizeof(e)) != 0) {
        finish_err("%s", e);
        json_decref(res);
        json_decref(applied);
        return;
    }
    wlog("air-assist offset %s counts", val);
    progress(100);
    finish_ok(1, res, applied);
    json_decref(res);
    json_decref(applied);
}

static void run_cooling_flow(void)
{
    char orig_pct[32] = "", orig_rise[32] = "";
    settings_get("cool_flow_heater_pct", orig_pct, sizeof(orig_pct));
    settings_get("cool_flow_rise", orig_rise, sizeof(orig_rise));
    double duty = wizcalc_next_duty(0.0);
    int rung = 0;
    json_t *res = NULL;
    while (duty > 0.0) {
        rung++;
        char d[16];
        snprintf(d, sizeof(d), "%.0f", duty);
        if (settings_set("cool_flow_heater_pct", d) != 0) {
            finish_err("cannot set the heater duty");
            goto restore;
        }
        wlog("calibration at %s percent heater duty (rung %d)", d, rung);
        phase("flow calibration at %s percent: settling the loop", d);
        res = run_diag("flow-calibrate", 1200, 2 + (rung - 1) * 30, 2 + rung * 30);
        if (!res)
            goto restore;
        const char *err = json_string_value(json_object_get(res, "error"));
        if (!err)
            break;
        if (strstr(err, "bands too close")) {
            wlog("gap %.1f C at %s percent: trying the next rung",
                 json_number_value(json_object_get(res, "gap")), d);
            json_decref(res);
            res = NULL;
            duty = wizcalc_next_duty(duty);
            continue;
        }
        finish_err("%s", err);
        json_decref(res);
        goto restore;
    }
    if (!res) {
        finish_err("the flow bands did not separate at 40, 55, or 70 percent: check the pump "
                   "and the coolant level, then run again");
        goto restore;
    }
    /* The threshold and the duty it was found at are one write; the
     * duty is already in force, so its before value is the original. */
    double rec = json_number_value(json_object_get(res, "recommend"));
    char v_rise[24], v_pct[16];
    snprintf(v_rise, sizeof(v_rise), "%.1f", rec);
    snprintf(v_pct, sizeof(v_pct), "%.0f", duty);
    json_t *applied = json_object();
    json_t *e = json_object();
    json_object_set_new(e, "from", json_string(orig_pct));
    json_object_set_new(e, "to", json_string(v_pct));
    json_object_set_new(applied, "cool_flow_heater_pct", e);
    const char *keys[] = { "cool_flow_rise" };
    const char *vals[] = { v_rise };
    char err[96];
    if (write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        finish_err("%s", err);
        json_decref(res);
        json_decref(applied);
        goto restore;
    }
    json_object_set_new(res, "heater_pct", json_real(duty));
    json_object_set_new(res, "threshold", json_real(rec));
    wlog("flow threshold %s C at %s percent heater duty", v_rise, v_pct);
    progress(100);
    finish_ok(1, res, applied);
    json_decref(res);
    json_decref(applied);
    return;
restore:
    if (orig_pct[0])
        settings_set("cool_flow_heater_pct", orig_pct);
}

static void run_cooling_flow_verify(void)
{
    phase("the flow check: settling the loop");
    json_t *res = run_diag("flow-verify", 240, 2, 95);
    if (!res)
        return;
    const char *err = json_string_value(json_object_get(res, "error"));
    if (err) {
        finish_err("%s", err);
        json_decref(res);
        return;
    }
    int pass = json_is_true(json_object_get(res, "pass"));
    int thin = json_is_true(json_object_get(res, "thin_margin"));
    if (!pass) {
        finish_err("the flow check does not separate flow from no-flow at the threshold: run the "
                   "calibration");
        commission_flag("cooling.flow", "required", "the flow check failed to separate the bands");
        json_decref(res);
        return;
    }
    if (thin) {
        wlog("the margin is thin: a calibration is recommended");
        commission_flag("cooling.flow", "recommended", "the flow check margin is thin");
    }
    progress(100);
    finish_ok(1, res, NULL);
    json_decref(res);
}

static void run_cooling_tec(void)
{
    int stopped = 0, took = 0;
    json_t *r = NULL, *applied = NULL;
    if (!commission_machine_bool("tec")) {
        r = json_object();
        json_object_set_new(r, "tec", json_false());
        json_object_set_new(r, "skipped", json_string("no TEC on this machine"));
        finish_ok(1, r, NULL);
        json_decref(r);
        return;
    }
    phase("stopping the motion controller");
    if (wiz_controller_stop() != 0) {
        finish_err("could not stop the motion controller");
        goto out;
    }
    stopped = 1;
    if (cool_diag_take() != 0) {
        finish_err("the cooling engine is busy");
        goto out;
    }
    took = 1;
    cool_diag_fans_run();
    msleep(3000);
    double tec0 = coolant_degc(rd_long("pic/tec_temp", 0));
    double up0 = coolant_degc(rd_long("pic/water_temp_2", 0));
    wlog("TEC %.1f C, upstream %.1f C before the drive", tec0, up0);
    phase("driving the TEC for 60 s");
    cool_diag_tec(1);
    double tec1 = tec0, up1 = up0;
    for (int i = 0; i < 30; i++) {
        if (aborted())
            goto out;
        msleep(2000);
        tec1 = coolant_degc(rd_long("pic/tec_temp", 0));
        up1 = coolant_degc(rd_long("pic/water_temp_2", 0));
        progress(10 + i * 3);
    }
    cool_diag_tec(0);
    double fall = (tec0 - up0) - (tec1 - up1);
    wlog("TEC %.1f C, upstream %.1f C after 60 s: %.1f C of relative fall", tec1, up1, fall);
    r = json_object();
    json_object_set_new(r, "tec_before_c", json_real(round(tec0 * 10) / 10));
    json_object_set_new(r, "tec_after_c", json_real(round(tec1 * 10) / 10));
    json_object_set_new(r, "relative_fall_c", json_real(round(fall * 10) / 10));
    int drives = fall >= 1.0;
    json_object_set_new(r, "tec", json_boolean(drives));
    const char *keys[] = { "cool_tec_present" };
    const char *vals[] = { drives ? "1" : "0" };
    applied = json_object();
    char err[96];
    cool_diag_fans_idle();
    cool_diag_release();
    took = 0;
    wiz_controller_start();
    stopped = 0;
    if (write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        finish_err("%s", err);
        goto out;
    }
    if (!drives)
        wlog("WARNING: the TEC did not cool by 1 C in 60 s; cool_tec_present cleared");
    progress(100);
    finish_ok(1, r, applied);
out:
    if (took) {
        cool_diag_tec(0);
        cool_diag_fans_idle();
        cool_diag_release();
    }
    if (stopped)
        wiz_controller_start();
    if (r)
        json_decref(r);
    if (applied)
        json_decref(applied);
}

/* ---------------------------------------------------------- the motion */

/* The Grbl socket in loopback posture: one line, its answer. */
static int grbl_connect(void)
{
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(23),
                                   .sin6_addr = IN6ADDR_LOOPBACK_INIT };
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(23),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read lines until one starts with `until` (or "error"/"ALARM"), at most
 * timeout_s. The line is left in out. 0 seen, -1 error or timeout. */
static int grbl_expect(int fd, const char *until, char *out, size_t olen, int timeout_s)
{
    static char rx[512];
    static size_t rxn;
    double deadline = wall_s() + timeout_s;
    while (wall_s() < deadline) {
        char *nl;
        while ((nl = memchr(rx, '\n', rxn))) {
            *nl = '\0';
            char *line = rx;
            while (*line == '\r' || *line == ' ')
                line++;
            size_t rest = rxn - (size_t)(nl + 1 - rx);
            char keep[512];
            memcpy(keep, nl + 1, rest);
            if (out && olen)
                snprintf(out, olen, "%.*s", (int)olen - 1, line);
            int hit = !strncmp(line, until, strlen(until));
            int bad = !strncmp(line, "error", 5) || !strncmp(line, "ALARM", 5);
            memcpy(rx, keep, rest);
            rxn = rest;
            if (hit)
                return 0;
            if (bad)
                return -1;
        }
        struct timeval tv = { 0, 200000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
            ssize_t r = read(fd, rx + rxn, sizeof(rx) - 1 - rxn);
            if (r <= 0)
                return -1;
            rxn += (size_t)r;
            rx[rxn] = '\0';
            if (rxn == sizeof(rx) - 1)
                rxn = 0;
        }
        if (aborted())
            return -1;
    }
    return -1;
}

static int grbl_send(int fd, const char *line)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    return write(fd, buf, (size_t)n) == n ? 0 : -1;
}

/* The head accelerometer over a window: the extremes and the RMS about
 * the mean on each axis, from the armed part's own outputs. */
typedef struct {
    long n;
    long minx, maxx, miny, maxy;            /* raw extremes */
    double sx, sxx, sy, syy;
    long bx[JOG_LP_N], by[JOG_LP_N];        /* the last JOG_LP_N samples */
    long lpminx, lpmaxx, lpminy, lpmaxy;    /* extremes of their average */
} accstat_t;

static void accstat_reset(accstat_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void accstat_add(accstat_t *s, long x, long y)
{
    if (s->n == 0) {
        s->minx = s->maxx = x;
        s->miny = s->maxy = y;
    } else {
        if (x < s->minx) s->minx = x;
        if (x > s->maxx) s->maxx = x;
        if (y < s->miny) s->miny = y;
        if (y > s->maxy) s->maxy = y;
    }
    s->sx += x;
    s->sxx += (double)x * x;
    s->sy += y;
    s->syy += (double)y * y;
    s->bx[s->n % JOG_LP_N] = x;
    s->by[s->n % JOG_LP_N] = y;
    s->n++;
    if (s->n >= JOG_LP_N) {
        long ax = 0, ay = 0;
        for (int i = 0; i < JOG_LP_N; i++) {
            ax += s->bx[i];
            ay += s->by[i];
        }
        ax /= JOG_LP_N;
        ay /= JOG_LP_N;
        if (s->n == JOG_LP_N) {
            s->lpminx = s->lpmaxx = ax;
            s->lpminy = s->lpmaxy = ay;
        } else {
            if (ax < s->lpminx) s->lpminx = ax;
            if (ax > s->lpmaxx) s->lpmaxx = ax;
            if (ay < s->lpminy) s->lpminy = ay;
            if (ay > s->lpmaxy) s->lpmaxy = ay;
        }
    }
}

static long accstat_lp_x(const accstat_t *s) { return s->n >= JOG_LP_N ? s->lpmaxx - s->lpminx : 0; }
static long accstat_lp_y(const accstat_t *s) { return s->n >= JOG_LP_N ? s->lpmaxy - s->lpminy : 0; }

static double accstat_rms(double sum, double sumsq, long n)
{
    if (n < 2)
        return 0.0;
    double mean = sum / n, var = sumsq / n - mean * mean;
    return var > 0.0 ? sqrt(var) : 0.0;
}

static double accstat_rms_x(const accstat_t *s) { return accstat_rms(s->sx, s->sxx, s->n); }
static double accstat_rms_y(const accstat_t *s) { return accstat_rms(s->sy, s->syy, s->n); }

/* Sample for `secs`; a failed read is skipped. */
/* Sample the head accelerometer for `secs` into s; with no s, only
 * wait (the callers outside the motion check have no watch armed). */
static void accstat_sample_for(accstat_t *s, double secs)
{
    double until = wall_s() + secs;
    if (!s) {
        while (wall_s() < until && !aborted())
            msleep(10);
        return;
    }
    while (wall_s() < until && !aborted()) {
        long x, y;
        if (crash_hw_sample(&x, &y) == 0)
            accstat_add(s, x, y);
        msleep(1);
    }
}

static json_t *accstat_json(const accstat_t *s)
{
    json_t *o = json_object();
    json_object_set_new(o, "samples", json_integer(s->n));
    json_object_set_new(o, "p2p_lp_x", json_integer(accstat_lp_x(s)));
    json_object_set_new(o, "p2p_lp_y", json_integer(accstat_lp_y(s)));
    json_object_set_new(o, "p2p_x", json_integer(s->n ? s->maxx - s->minx : 0));
    json_object_set_new(o, "p2p_y", json_integer(s->n ? s->maxy - s->miny : 0));
    json_object_set_new(o, "rms_x", json_real(round(accstat_rms_x(s))));
    json_object_set_new(o, "rms_y", json_real(round(accstat_rms_y(s))));
    return o;
}

/* Wait for the controller to report Idle, sampling the head
 * accelerometer meanwhile into `st`. */
static int grbl_wait_idle(int fd, int timeout_s, accstat_t *st)
{
    double deadline = wall_s() + timeout_s;
    char line[160];
    while (wall_s() < deadline) {
        accstat_sample_for(st, 0.06);
        if (write(fd, "?", 1) != 1)
            return -1;
        if (grbl_expect(fd, "<", line, sizeof(line), 2) == 0 && !strncmp(line, "<Idle", 5))
            break;
        if (aborted())
            return -1;
    }
    return wall_s() < deadline ? 0 : -1;
}

static int kernel_idle(void)
{
    char path[192], text[32];
    snprintf(path, sizeof(path), "%s/cnc/state", sysfs_root());
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    text[n] = '\0';
    return strncmp(text, "idle", 4) == 0;
}

/* Then wait for the pulse engine to finish playing it, still sampling:
 * the controller's Idle comes when the stream is produced, and the
 * kernel plays the rest of it afterward. */
static int kernel_wait_idle(double timeout_s, accstat_t *st)
{
    double deadline = wall_s() + timeout_s;
    while (wall_s() < deadline) {
        int idle = kernel_idle();
        if (idle < 0)
            return -1;
        if (idle)
            return 0;
        accstat_sample_for(st, 0.02);
        if (aborted())
            return -1;
    }
    return -1;
}

/* The lens hall reference, as the factory's Z homing does it: step
 * away until the hall reads home, then toward the bed until it does
 * not, five passes; the passes must agree. Direct z_step pulses with
 * the controller stopped. */
static int z_step_until(int at_home, int *count)
{
    *count = 0;
    for (int i = 0; i < 200; i++) {
        long h = rd_long("head/hall_sensor", -1);
        if (h < 0)
            return -1;
        int home = h == 0;      /* the kernel reads 0 at home (gfhardware) */
        if (home == at_home)
            return 0;
        if (wr_attr("cnc/z_step", at_home ? "1" : "0") != 0)
            return -1;
        (*count)++;
        msleep(180);
        if (aborted())
            return -1;
    }
    return -2;
}

static int z_reference(json_t *r, int npass)
{
    wr_attr("head/z_current", "0");     /* high */
    wr_attr("head/z_mode", "0");        /* full step */
    wr_attr("head/z_enable", "0");      /* enabled */
    int c, rc;
    long pos = 0, passes[5];
    if (npass < 1)
        npass = 1;
    if (npass > 5)
        npass = 5;
    if ((rc = z_step_until(0, &c)) != 0 || (rc = z_step_until(1, &c)) != 0)
        goto done;
    for (int p = 0; p < npass; p++) {
        phase("the lens finds its reference on the hall sensor: pass %d of %d", p + 1, npass);
        if ((rc = z_step_until(0, &c)) != 0)
            goto done;
        pos -= c * 2;
        if ((rc = z_step_until(1, &c)) != 0)
            goto done;
        pos += c * 2;
        passes[p] = pos;
        wlog("lens reference pass %d: %ld", p + 1, pos);
        progress(60 + p * 6);
    }
    json_t *arr = json_array();
    for (int p = 0; p < npass; p++)
        json_array_append_new(arr, json_integer(passes[p]));
    json_object_set_new(r, "z_passes", arr);
    rc = wizcalc_z_concur(passes, npass, 4) ? 0 : -3;
done:
    wr_attr("head/z_current", "1");     /* low */
    wr_attr("head/z_mode", "1");        /* half step */
    return rc;
}

static int grbl_up(void *ctx)
{
    (void)ctx;
    int fd = grbl_connect();
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static void run_motion(void)
{
    int stopped = 0, local = 0, watch = 0, fd = -1;
    json_t *r = json_object();
    phase("stopping the motion controller");
    if (wiz_controller_stop() != 0) {
        finish_err("could not stop the motion controller");
        goto out;
    }
    stopped = 1;
    phase("rail up, the liveness probe");
    char detail[96];
    int rc = super_probe_motion(detail, sizeof(detail));
    wlog("liveness probe: %s", detail);
    json_object_set_new(r, "probe", json_string(detail));
    if (rc != 1) {
        finish_err(rc == 2 ? "the probe could not run: %s" : "no motion on the probe: %s", detail);
        goto out;
    }
    progress(20);
    if (aborted())
        goto out;
    phase("the lens hall reference");
    rc = z_reference(r, 5);
    if (rc != 0) {
        finish_err(rc == -2 ? "the lens never reached the hall sensor in 200 steps"
                   : rc == -3 ? "the lens reference passes disagree"
                              : "the lens reference could not run");
        goto out;
    }
    json_object_set_new(r, "z_referenced", json_true());
    progress(40);
    /* grblHAL in loopback posture for the jogs. */
    super_set_local(1);
    local = 1;
    wiz_controller_start();
    stopped = 0;
    phase("starting the controller in loopback posture");
    if (wait_for(grbl_up, NULL, 60) != 0) {
        finish_err("the controller did not come up for the jogs");
        goto out;
    }
    fd = grbl_connect();
    if (fd < 0) {
        finish_err("cannot reach the controller on the Grbl socket");
        goto out;
    }
    char line[160];
    grbl_expect(fd, "Grbl", line, sizeof(line), 5);
    grbl_send(fd, "$X");
    grbl_expect(fd, "ok", line, sizeof(line), 5);
    /* The crash watch is armed for the jogs, and its i2c path is the
     * witness's sampler, so the part must answer. */
    crash_watch_t cw;
    crash_reset(&cw);
    if (crash_hw_open() != 0) {
        finish_err("the head accelerometer does not answer on its i2c bus");
        goto out;
    }
    if (crash_hw_arm((int)gate_value("cool_accel_x_alert"), (int)gate_value("cool_accel_y_alert"),
                     (int)gate_value("cool_accel_abort")) != 0) {
        crash_hw_close();
        finish_err("the head accelerometer could not be armed");
        goto out;
    }
    watch = 1;
    if (ask_continue("jogs", "The head moves 50 mm each way on X, then on Y, at 3000 mm/min. "
                             "Keep the bed clear and the lid closed.") != 0)
        goto out;
    phase("the head at rest");
    accstat_t rest;
    accstat_reset(&rest);
    accstat_sample_for(&rest, JOG_REST_S);
    if (aborted())
        goto out;
    double rest_x = accstat_lp_x(&rest), rest_y = accstat_lp_y(&rest);
    wlog("at rest: accelerometer low-passed p2p x %.0f y %.0f (rms %.0f %.0f) over %ld samples",
         rest_x, rest_y, accstat_rms_x(&rest), accstat_rms_y(&rest), rest.n);
    json_object_set_new(r, "rest", accstat_json(&rest));
    if (rest.n < 20) {
        finish_err("the head accelerometer gave %ld samples at rest, not enough to judge a jog",
                   rest.n);
        goto out;
    }
    static const char *const jogs[] = { "$J=G91 G21 X50 F3000", "$J=G91 G21 X-50 F3000",
                                        "$J=G91 G21 Y50 F3000", "$J=G91 G21 Y-50 F3000" };
    static const char *const names[] = { "+X", "-X", "+Y", "-Y" };
    json_t *moves = json_object();
    for (int i = 0; i < 4; i++) {
        if (aborted())
            goto out;
        phase("jog %s", names[i]);
        if (grbl_send(fd, jogs[i]) != 0 || grbl_expect(fd, "ok", line, sizeof(line), 5) != 0) {
            finish_err("the controller refused the %s jog: %s", names[i], line);
            json_decref(moves);
            goto out;
        }
        accstat_t st;
        accstat_reset(&st);
        if (grbl_wait_idle(fd, 15, &st) != 0) {
            finish_err("the %s jog did not finish", names[i]);
            json_decref(moves);
            goto out;
        }
        if (kernel_wait_idle(JOG_DRAIN_S, &st) != 0) {
            finish_err("the pulse engine did not finish playing the %s jog", names[i]);
            json_decref(moves);
            goto out;
        }
        unsigned s1 = 0, s2 = 0;
        int crash = 0;
        if (crash_hw_poll(&s1, &s2) == 0) {
            crash_event_t e = crash_tick(&cw, s1, s2);
            crash = e == CrashEv_Alarm;         /* the fail tier; a jog rings the alert tier */
        }
        double lx = accstat_lp_x(&st), ly = accstat_lp_y(&st);
        int seen = wizcalc_jog_witnessed(lx, ly, rest_x, rest_y, JOG_LP_RATIO, JOG_LP_FLOOR);
        wlog("%s: accelerometer low-passed p2p x %.0f y %.0f (rms %.0f %.0f, raw p2p %ld %ld, "
             "%ld samples)%s", names[i], lx, ly, accstat_rms_x(&st), accstat_rms_y(&st),
             st.n ? st.maxx - st.minx : 0, st.n ? st.maxy - st.miny : 0, st.n,
             crash ? " CRASH EVENT" : "");
        json_t *m = accstat_json(&st);
        json_object_set_new(m, "witnessed", json_boolean(seen));
        json_object_set_new(m, "crash_event", json_boolean(crash));
        json_object_set_new(moves, names[i], m);
        /* The crash watch's abort tier trips on a commanded jog, which is
         * exactly the head motion it is built to catch; the event is
         * recorded but does not fail the check. The accelerometer is the
         * witness that the head physically moved. */
        if (!seen) {
            json_object_set_new(r, "moves", moves);
            finish_err("the accelerometer did not witness the %s jog (low-passed p2p %.0f, "
                       "at rest %.0f; moving is %.1f times rest and %.0f or more)",
                       names[i], lx > ly ? lx : ly, rest_x > rest_y ? rest_x : rest_y,
                       JOG_LP_RATIO, JOG_LP_FLOOR);
            goto out;
        }
        progress(45 + (i + 1) * 12);
    }
    json_object_set_new(r, "moves", moves);
    json_object_set_new(r, "rail", json_string("up"));
    progress(100);
    finish_ok(1, r, NULL);
out:
    if (watch) {
        crash_hw_disarm();
        crash_hw_close();
    }
    if (fd >= 0)
        close(fd);
    if (local) {
        wiz_controller_stop();
        super_set_local(0);
        wiz_controller_start();
    } else if (stopped)
        wiz_controller_start();
    json_decref(r);
}

/* --------------------------------------------------------- the cameras */

static int shot(cam_id_t cam, const char *name)
{
    uint8_t *jpeg = NULL;
    size_t len = 0;
    char err[128] = "";
    if (cam_snapshot(cam, 0, 80, -1, &jpeg, &len, err, sizeof(err)) != 0) {
        wlog("%s snapshot: %s", name, err);
        return -1;
    }
    pthread_mutex_lock(&mu);
    free(shot_jpeg[cam]);
    shot_jpeg[cam] = jpeg;
    shot_len[cam] = len;
    pthread_mutex_unlock(&mu);
    wlog("%s snapshot: %zu bytes", name, len);
    return 0;
}

static void run_cameras(void)
{
    json_t *r = json_object();
    if (!machine_lid_closed()) {
        if (edge("lid-close", "Close the lid: the cameras capture only with it closed.", 3, 1,
                 PROMPT_TIMEOUT_S) != 0)
            goto out;
    }
    struct cam_status st;
    cam_get_status(&st);
    json_object_set_new(r, "sensor", json_string(st.sensor && st.sensor[0] ? st.sensor : "unknown"));
    json_object_set_new(r, "lamp_idle", json_integer(cam_lamp_idle_level()));
    phase("a lid camera snapshot");
    progress(10);
    if (shot(CAM_LID, "lid") != 0) {
        finish_err("the lid camera did not capture");
        goto out;
    }
    progress(40);
    int ok = ask_confirm("lid-view", "This is the lid camera. Can you see the bed?");
    if (ok < 0)
        goto out;
    json_object_set_new(r, "lid_ok", json_boolean(ok));
    if (!ok) {
        finish_err("the lid camera view was not accepted");
        goto out;
    }
    phase("a head camera snapshot");
    if (shot(CAM_HEAD, "head") != 0) {
        finish_err("the head camera did not capture");
        goto out;
    }
    progress(75);
    ok = ask_confirm("head-view", "This is the head camera. Can you see the bed under the lens?");
    if (ok < 0)
        goto out;
    json_object_set_new(r, "head_ok", json_boolean(ok));
    if (!ok) {
        finish_err("the head camera view was not accepted");
        goto out;
    }
    progress(100);
    finish_ok(1, r, NULL);
out:
    json_decref(r);
}

/* ----------------------------------------------------- the cloud header */

#define CAPTURE_MARKER "/run/gfcloud-capture"
#define CAPTURE_FILE_NAME "cloud-header.json"

/* The calibration-bearing tag families the record keeps beside the
 * machine's own numbers: fans and their tach windows, the coolant
 * windows, the step axes, the purge fan, the lid IR and head
 * accelerometer thresholds, the HV current caps. */
static const char *const header_families[] = {
    "AA", "EF", "IF", "CM", "CT", "CF", "XS", "YS", "ZS", "PA", "LI", "HA", "HV",
};

static int capture_present(void *ctx)
{
    return access((const char *)ctx, F_OK) == 0;
}

static int mode_is(void *ctx)
{
    char buf[512];
    if (super_status_json(buf, sizeof(buf)) < 0)
        return 0;
    json_t *m = json_loads(buf, 0, NULL);
    if (!m)
        return 0;
    const char *mode = json_string_value(json_object_get(m, "mode"));
    const char *ctl = json_string_value(json_object_get(m, "controller"));
    int ok = mode && ctl && !strcmp(mode, (const char *)ctx) && !strcmp(ctl, "running");
    json_decref(m);
    return ok;
}

static void run_cloud_header(void)
{
    json_t *r = json_object();
    char capture[192], err[96];
    int posture = 0, in_cloud = 0;
    snprintf(capture, sizeof(capture), "%s/%s", ff_run_dir(), CAPTURE_FILE_NAME);
    if (!settings_get_bool("cloud_enabled", 0)) {
        json_object_set_new(r, "skipped", json_string("cloud mode is off"));
        finish_ok(1, r, NULL);
        json_decref(r);
        return;
    }
    unlink(capture);
    FILE *m = fopen(CAPTURE_MARKER, "w");
    if (!m) {
        finish_err("cannot write the capture marker");
        goto out;
    }
    fclose(m);
    /* The wizard owns the controller: loopback posture keeps the gate
     * from stopping what it starts, and the cloud client is the
     * controller it starts. */
    super_set_local(1);
    posture = 1;
    phase("starting the cloud client");
    led_release();                  /* the cloud client drives the button */
    if (super_mode_switch("cloud", err, sizeof(err)) != 0) {
        finish_err("cannot start cloud mode: %s", err);
        goto out;
    }
    in_cloud = 1;
    if (wait_for(mode_is, (void *)"cloud", 120) != 0) {
        finish_err("the cloud client did not come up");
        goto out;
    }
    progress(20);
    if (ask_continue("print", "The machine is in cloud mode. In the Glowforge app, place any small "
                              "design on the bed image and press Print. Do not press the machine's "
                              "button: the wizard takes the print's header and cancels it.") != 0)
        goto out;
    phase("waiting for the print's header from the service");
    int rc = wait_for(capture_present, capture, 600);
    if (rc != 0) {
        finish_err(rc == -2 ? "no header arrived within 10 min" : "aborted");
        goto out;
    }
    progress(70);
    json_error_t jerr;
    json_t *doc = json_load_file(capture, 0, &jerr);
    if (!doc) {
        finish_err("the capture did not parse: %s", jerr.text);
        goto out;
    }
    json_t *tags = json_object_get(doc, "tags");
    json_t *kept = json_object();
    const char *k;
    json_t *v;
    int n = 0, total = 0;
    json_object_foreach(tags, k, v) {
        total++;
        for (size_t i = 0; i < sizeof(header_families) / sizeof(*header_families); i++)
            if (!strncmp(k, header_families[i], 2)) {
                json_object_set(kept, k, v);
                n++;
                break;
            }
    }
    json_object_set_new(r, "captured", json_incref(json_object_get(doc, "captured")));
    json_object_set_new(r, "job_id", json_incref(json_object_get(doc, "job_id")));
    json_object_set_new(r, "tag_count", json_integer(total));
    json_object_set_new(r, "calibration_tags", kept);
    json_object_set(r, "limits", json_object_get(doc, "limits"));
    /* The machine's own numbers beside the service's. */
    json_t *mine = json_object();
    static const char *const keys[] = {
        "cool_tach_exhaust_min_rpm", "cool_tach_intake_min_rpm", "cool_tach_air_assist_min_rpm",
        "cool_purge_min_current", "cool_temp_max", "cool_temp_min", "cool_flow_rise",
        "cool_fire_q1_alert", "cool_fire_q2_alert", "cool_accel_x_alert", "cool_accel_y_alert",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        char val[64] = "";
        settings_get(keys[i], val, sizeof(val));
        const gate_setting_t *g = gate_setting_find(keys[i]);
        json_t *e = json_object();
        json_object_set_new(e, "local", json_string(val));
        if (g)
            json_object_set_new(e, "default", json_real(g->def));
        json_object_set_new(mine, keys[i], e);
    }
    json_object_set_new(r, "machine", mine);
    char *block = json_dumps(kept, JSON_COMPACT | JSON_SORT_KEYS);
    fflog(LOG_INFO, "cloud.header: factory envelope captured, %d of %d tags kept: %s", n, total,
          block ? block : "");
    free(block);
    json_decref(doc);
    unlink(capture);
    wlog("factory envelope captured: %d tags, %d calibration-bearing", total, n);
    progress(85);
    /* The client writes the capture once the print's cancel is queued
     * for the service; the wire gets a moment before the client is
     * stopped, so the app is released and not left waiting. */
    phase("the cloud client cancels the print");
    for (int i = 0; i < 30 && !aborted(); i++)
        msleep(100);
    phase("back to GRBL mode");
    super_mode_switch("grbl", err, sizeof(err));
    in_cloud = 0;
    super_set_local(0);
    posture = 0;
    progress(100);
    finish_ok(1, r, NULL);
out:
    unlink(CAPTURE_MARKER);
    if (in_cloud)
        super_mode_switch("grbl", err, sizeof(err));
    if (posture)
        super_set_local(0);
    json_decref(r);
}

/* ------------------------------------------------------------ runner */

static const wiz_entry_t wizards[] = {
    { "cloud.header",        run_cloud_header,        1 },
    { "switches",            run_switches,            0 },
    { "sensors",             run_sensors,             1 },
    { "airflow",             run_airflow,             1 },
    { "cooling.aa-offset",   run_cooling_aa,          1 },
    { "cooling.flow",        run_cooling_flow,        1 },
    { "cooling.flow-verify", run_cooling_flow_verify, 1 },
    { "cooling.tec",         run_cooling_tec,         1 },
    { "motion",              run_motion,              1 },
    { "cameras",             run_cameras,             1 },
};
#define NWIZ (sizeof(wizards) / sizeof(*wizards))

/* The entry for an id: the dark table first, then the live one
 * (wizlive.c), as one index space. */
static const wiz_entry_t *entry(int ix)
{
    if (ix < 0)
        return NULL;
    if ((size_t)ix < NWIZ)
        return &wizards[ix];
    if ((size_t)ix - NWIZ < wizlive_n)
        return &wizlive_table[ix - (int)NWIZ];
    return NULL;
}

static int find(const char *id)
{
    for (size_t i = 0; i < NWIZ; i++)
        if (!strcmp(wizards[i].id, id))
            return (int)i;
    for (size_t i = 0; i < wizlive_n; i++)
        if (!strcmp(wizlive_table[i].id, id))
            return (int)(NWIZ + i);
    return -1;
}

int wizdark_known(const char *id)
{
    return id && find(id) >= 0;
}

static void *worker(void *arg)
{
    int ix = (int)(intptr_t)arg;
    entry(ix)->run();
    /* Every run puts the controller back, which hands the LED over; a
     * run that ended still holding the machine gives the light back
     * here. */
    if (machine_held) {
        machine_held = 0;
        led_release();
    }
    pthread_mutex_lock(&mu);
    if (S.abort_req && !S.error[0])
        snprintf(S.error, sizeof(S.error), "aborted");
    S.running = 0;
    S.prompt_seq = 0;
    S.phase[0] = '\0';
    S.owner[0] = '\0';
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int wizdark_start(const char *id, const char *owner, char *err, size_t elen)
{
    int ix = find(id ? id : "");
    if (ix < 0) {
        snprintf(err, elen, "no such wizard");
        return -1;
    }
    pthread_mutex_lock(&mu);
    if (S.running) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a wizard is already running (%s)", S.id);
        return -1;
    }
    if (diag_running()) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a diagnostic holds the machine");
        return -1;
    }
    if (entry(ix)->needs_idle && !machine_is_idle()) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "the machine is not idle");
        return -1;
    }
    snprintf(S.id, sizeof(S.id), "%s", id);
    S.running = 1;
    S.abort_req = 0;
    S.progress = 0;
    S.phase[0] = '\0';
    S.error[0] = '\0';
    S.log_n = 0;
    S.prompt_seq = 0;
    S.answer_seq = 0;
    S.started = time(NULL);
    snprintf(S.owner, sizeof(S.owner), "%s", owner ? owner : "");
    if (S.result) {
        json_decref(S.result);
        S.result = NULL;
    }
    if (S.applied) {
        json_decref(S.applied);
        S.applied = NULL;
    }
    pthread_mutex_unlock(&mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, worker, (void *)(intptr_t)ix) != 0) {
        pthread_attr_destroy(&at);
        pthread_mutex_lock(&mu);
        S.running = 0;
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "cannot start the wizard thread");
        return -1;
    }
    pthread_attr_destroy(&at);
    fflog(LOG_NOTICE, "wiz %s: started", id);
    return 0;
}

int wizdark_answer(const char *id, int seq, const char *value, const char *requester)
{
    pthread_mutex_lock(&mu);
    if (S.running && !wizcalc_may_act(S.owner, requester)) {
        pthread_mutex_unlock(&mu);
        return -2;
    }
    if (!S.running || strcmp(S.id, id ? id : "") || S.prompt_seq == 0 || seq != S.prompt_seq) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    snprintf(S.answer, sizeof(S.answer), "%s", value ? value : "");
    S.answer_seq = seq;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 0;
}

int wizdark_abort(const char *requester)
{
    pthread_mutex_lock(&mu);
    if (S.running && !wizcalc_may_act(S.owner, requester)) {
        pthread_mutex_unlock(&mu);
        return -2;
    }
    if (S.running) {
        S.abort_req = 1;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

void wizdark_take_over(const char *requester)
{
    pthread_mutex_lock(&mu);
    if (S.running && requester && *requester &&
        strcmp(S.owner, requester)) {
        snprintf(S.owner, sizeof(S.owner), "%s", requester);
        fflog(LOG_NOTICE, "wiz %s: another browser took the run over", S.id);
    }
    pthread_mutex_unlock(&mu);
}

int wizdark_running(void)
{
    pthread_mutex_lock(&mu);
    int r = S.running;
    pthread_mutex_unlock(&mu);
    return r;
}

int wizdark_wraps_diag(void)
{
    pthread_mutex_lock(&mu);
    int r = S.running && !strncmp(S.id, "cooling.", 8) && strcmp(S.id, "cooling.tec");
    pthread_mutex_unlock(&mu);
    return r;
}

int wizdark_status_json(char *buf, size_t len, const char *requester)
{
    pthread_mutex_lock(&mu);
    json_t *o = json_object();
    json_object_set_new(o, "id", json_string(S.id));
    json_object_set_new(o, "running", json_boolean(S.running));
    json_object_set_new(o, "owned", json_boolean(S.running && S.owner[0]));
    json_object_set_new(o, "mine", json_boolean(!S.running || wizcalc_may_act(S.owner, requester)));
    json_object_set_new(o, "phase", json_string(S.phase));
    json_object_set_new(o, "progress", json_integer(S.progress));
    json_object_set_new(o, "elapsed_s",
                        json_integer(S.running || S.id[0] ? (long)(time(NULL) - S.started) : 0));
    json_t *log = json_array();
    for (int i = 0; i < S.log_n; i++)
        json_array_append_new(log, json_string(S.log[i]));
    json_object_set_new(o, "log", log);
    if (S.running && S.prompt_seq) {
        json_t *p = json_object();
        json_object_set_new(p, "seq", json_integer(S.prompt_seq));
        json_object_set_new(p, "id", json_string(S.prompt_id));
        json_object_set_new(p, "kind", json_string(S.prompt_kind));
        json_object_set_new(p, "text", json_string(S.prompt_text));
        json_t *opts = json_array();
        for (int i = 0; i < S.prompt_nopt; i++)
            json_array_append_new(opts, json_string(S.prompt_opt[i]));
        json_object_set_new(p, "options", opts);
        json_object_set_new(p, "timeout_s", json_integer(S.prompt_timeout));
        json_object_set_new(p, "since_s", json_integer((long)(time(NULL) - S.prompt_opened)));
        json_object_set_new(o, "prompt", p);
    } else
        json_object_set_new(o, "prompt", json_null());
    json_object_set(o, "result", S.result ? S.result : json_null());
    json_object_set(o, "applied", S.applied ? S.applied : json_null());
    json_object_set_new(o, "error", json_string(S.error));
    json_object_set_new(o, "shots", json_pack("{s:b,s:b}", "lid", shot_jpeg[CAM_LID] != NULL,
                                              "head", shot_jpeg[CAM_HEAD] != NULL));
    pthread_mutex_unlock(&mu);
    char *text = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    if (!text)
        return -1;
    int n = snprintf(buf, len, "%s", text);
    free(text);
    return n < (int)len ? n : -1;
}

uint8_t *wizdark_shot(const char *cam, size_t *len)
{
    int ix = !strcmp(cam ? cam : "", "head") ? CAM_HEAD : CAM_LID;
    pthread_mutex_lock(&mu);
    uint8_t *out = NULL;
    if (shot_jpeg[ix]) {
        out = malloc(shot_len[ix]);
        if (out) {
            memcpy(out, shot_jpeg[ix], shot_len[ix]);
            *len = shot_len[ix];
        }
    }
    pthread_mutex_unlock(&mu);
    return out;
}

void wizdark_init(void)
{
    memset(&S, 0, sizeof(S));
}

void wizdark_shutdown(void)
{
    wizdark_abort(NULL);
    pthread_mutex_lock(&mu);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    while (S.running && pthread_cond_timedwait(&cv, &mu, &until) != ETIMEDOUT)
        ;
    pthread_mutex_unlock(&mu);
}

/* ------------------------------------------------ shared with wizlive.c */

void wiz_log(const char *fmt, ...)
{
    char line[92];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wlog("%s", line);
}

void wiz_phase(const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    phase("%s", text);
}

void wiz_progress(int pct) { progress(pct); }
int wiz_aborted(void) { return aborted(); }
const volatile int *wiz_abort_flag(void) { return &S.abort_req; }
void wiz_msleep(int ms) { msleep(ms); }
int wiz_wait_for(int (*cond)(void *), void *ctx, int timeout_s) { return wait_for(cond, ctx, timeout_s); }

int wiz_ask(const char *kind, const char *pid, const char *text, const char *const *opts,
            int nopt, char *answer, size_t alen)
{
    return ask(kind, pid, text, opts, nopt, answer, alen);
}
int wiz_ask_t(const char *kind, const char *pid, const char *text, const char *const *opts,
              int nopt, int timeout_s, char *answer, size_t alen)
{
    return ask_t(kind, pid, text, opts, nopt, timeout_s, answer, alen);
}
int wiz_ask_continue(const char *pid, const char *text) { return ask_continue(pid, text); }
int wiz_ask_confirm(const char *pid, const char *text) { return ask_confirm(pid, text); }
void wiz_wait_open(const char *pid, const char *text, int timeout_s) { wait_open(pid, text, timeout_s); }
void wiz_wait_close(void) { wait_close(); }

void wiz_finish_err(const char *fmt, ...)
{
    char text[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    finish_err("%s", text);
}

void wiz_finish_ok(int version, json_t *result, json_t *applied) { finish_ok(version, result, applied); }

int wiz_write_settings(const char *const *keys, const char *const *vals, size_t n,
                       json_t *applied, char *err, size_t elen)
{
    return write_settings(keys, vals, n, applied, err, elen);
}
double wiz_setting_num(const char *key, double def) { return setting_num(key, def); }
int wiz_imperial(void)
{
    char v[32];
    return settings_get("ui_units", v, sizeof(v)) == 0 && !strcmp(v, "imperial");
}
const char *wiz_len(double mm, char *buf, size_t n)
{
    if (wiz_imperial())
        snprintf(buf, n, "%.3f in", mm / 25.4);
    else
        snprintf(buf, n, "%.1f mm", mm);
    return buf;
}
double wiz_len_mm(const char *text)
{
    double v = strtod(text, NULL);
    return wiz_imperial() ? v * 25.4 : v;
}
long wiz_rd_long(const char *attr, long fallback) { return rd_long(attr, fallback); }
int wiz_wr_attr(const char *attr, const char *val) { return wr_attr(attr, val); }
int wiz_kernel_wait_idle(double timeout_s) { return kernel_wait_idle(timeout_s, NULL); }
int wiz_grbl_up(void *ctx) { return grbl_up(ctx); }
int wiz_grbl_connect(void) { return grbl_connect(); }
int wiz_grbl_send(int fd, const char *line) { return grbl_send(fd, line); }
int wiz_grbl_expect(int fd, const char *until, char *out, size_t olen, int timeout_s)
{
    return grbl_expect(fd, until, out, olen, timeout_s);
}
int wiz_grbl_wait_idle(int fd, int timeout_s) { return grbl_wait_idle(fd, timeout_s, NULL); }
int wiz_z_reference(json_t *r, int passes) { return z_reference(r, passes); }
