/*
 * cool_engine_test.c - host unit test for the cooling engine, tick by tick
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The cooling engine, driven tick by tick against a fake sysfs tree and
 * a fake clock, with a loop model that carries the run airflow's ADC
 * offset (about 1 C, stepping in with the run airflow and toggling
 * between two levels):
 *
 *   G. the air-assist offset: taken off both readings in counts while
 *                           the fan runs, in part at a part duty,
 *                           bounded, nothing at idle
 *
 * The same harness drives the verdict a coolant sensor read failure
 * produces (a sysfs error, not a rail value): the engine is blind on
 * every coolant gate, so it says so rather than fire on:
 *
 *   H. blind mid-run:       one failed read tolerated, two are SENSOR
 *                           (fire blocked, hold, heater off), released
 *                           the moment the reading is back
 *   J. blind under the floor: COLD keeps its state through SENSOR
 *   K. blind in a warm-up:  the heater goes off and comes back
 *   M. the hang dead-man:   a report under the timeout is still fresh and
 *                           acknowledged; silence while armed stops and
 *                           locks once; silence while the kernel runs
 *                           repeats every tick
 *   N. the fail tiers:      FIRE and CRASH end the controller through the
 *                           supervisor's hook, once, after the kernel writes
 *   O. the pause tiers:     FLAME and BUMP hold the job and call nothing
 *   Q. the verdict file:    the contract's keys and types; the stamp on
 *                           CLOCK_MONOTONIC to the millisecond, never
 *                           ahead of the clock
 *
 * The fan writes: every duty the engine writes is read back, tried again
 * when the device did not take it, and held to the command once a tick.
 * The fake tree's writes can be dropped (the device answers and keeps its
 * value) or refused (the bus fails):
 *
 *   fan writes A. clean:    a session opens and every fan reads its run duty
 *   fan writes B. dropped:  two writes dropped, the third takes; no fault
 *   fan writes C. lost:     a device loses its duty mid-session; the tick
 *                           puts it back and the fan gets its spin-up
 *                           grace, so a fan that then runs is not tripped
 *   fan writes D. refused:  a device takes nothing, or the bus refuses the
 *                           write; the fan is tripped at its tach and the
 *                           fault names the duty and the readback
 *   fan writes E. thermal:  the same readback route holds the exhaust PWM
 *
 * The intakes fitted: cool_intake_fans 1 (the setup's airflow check
 * writes it) takes the second intake tach out of the airflow gates and
 * changes nothing else. The fake tree's second intake tach reads no pulse:
 *
 *   intakes A. two fans:    the default judges the silent tach, trips
 *                           AIRFLOW on it, and never acknowledges the window
 *   intakes B. one fan:     no fault, the window is acknowledged, and
 *                           intake_2 is published not fitted with no floor
 *   intakes C. one fan:     a silent intake 1 still trips AIRFLOW
 *   intakes D. one fan:     with the intake gate off, intake_2 is still
 *                           not judged, and a fitted fan under the
 *                           default is still named
 *   intakes E. any other value (a hand-edited 0) keeps intake_2 judged
 *
 * The phases and the gates, each setting moved past the loop the way the
 * run start reads it:
 *
 *   phases A.  a closed session ends at the idle duties
 *   phases B.  a busy start takes the cooldown duties until the kernel is
 *              idle with no session, then the idle duties
 *   gates A.   the ceiling under the loop: OVERTEMP at the run start; at
 *              60 it is off, said so
 *   gates B.   the start gate over the loop: WARMUP with the heater on and
 *              the fans idle, released on the minute's minimum, not on a
 *              slug; both low gates at 0 are off
 *   gates C.   the critical line under the loop: CRITICAL, no resume, gone
 *              with the session; at 70 it is off and the ceiling pauses
 *   gates D.   a fan floor no fan meets: AIRFLOW after the grace and three
 *              ticks, gone with the session; a floor of 0 is off
 *   crash watch: never armed in an unarmed session; its thresholds at 0
 *              are its gates off
 *   TEC.       fitted: on with the run airflow, off at idle, a dwell before
 *              it comes on again; not fitted: never
 *
 * And after them: the holds that clear on their own light nothing; at most
 * five notices, their document JSON-safe and never cut; a waiting
 * acknowledgment keeps its record; the flow watch's context (its fans and
 * run session, the two corrections) and the yields that land at once (K3);
 * no engine heat outside a warm-up; the warnings that arm the flight
 * recorder; the retired flow-check settings; the shutdown's order.
 */
#define GF_SYSFS    "cool-engine-test/sys/"
#define VERDICT_DIR "cool-engine-test/run"
#define clock_gettime fake_clock_gettime
/* The head accelerometer's interrupt generators, faked: the engine's
   calls land here (the real ones open an i2c bus this host lacks). */
#define crash_hw_open   fake_crash_open
#define crash_hw_arm    fake_crash_arm
#define crash_hw_poll   fake_crash_poll
#define crash_hw_disarm fake_crash_disarm
#define crash_hw_close  fake_crash_close
/* The engine's start runs here without its thread: the cases tick it. */
#define pthread_create  fake_pthread_create
/* The fake tree is plain files: a write must truncate, as a sysfs store
 * replaces, or a short duty leaves the tail of a longer one for the
 * engine's readback to find. The one attribute a fan-write case names can
 * drop writes (the device answers, keeps its value) or refuse them (the
 * bus fails). */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
static const char *drop_attr;       /* a path suffix, or NULL */
static int drop_left;               /* writes to drop; -1 = every one */
static int refuse;                  /* refuse instead of dropping */
static int drop_fd = -1;

static int fake_open(const char *path, int flags, ...)
{
    int drop = 0;
    if ((flags & O_WRONLY) && drop_attr && drop_left != 0) {
        size_t lp = strlen(path), la = strlen(drop_attr);
        drop = lp >= la && !strcmp(path + lp - la, drop_attr);
    }
    /* A dropped write leaves the device's value where it was: no
     * truncation for that one. */
    int fd = openat(AT_FDCWD, path,
                    (flags & O_WRONLY) && !drop ? (flags | O_TRUNC) : flags, 0644);
    if (fd >= 0 && drop)
        drop_fd = fd;
    return fd;
}

static ssize_t fake_write(int fd, const void *buf, size_t n)
{
    if (fd == drop_fd && drop_left != 0) {
        if (drop_left > 0)
            drop_left--;
        drop_fd = -1;
        if (refuse) {
            errno = EIO;
            return -1;
        }
        return (ssize_t)n;          /* taken, as far as the writer sees */
    }
    return write(fd, buf, n);
}
#define open fake_open
#define write fake_write

#include "../src/cool.c"

#include <stdarg.h>
#include <sys/stat.h>

/* --- fakes ------------------------------------------------------------ */

static double fake_now = 1000.0;

/* The engine's clock is CLOCK_MONOTONIC. Any other clock reads far from
 * it, so a stamp or a timeout taken from another clock shows. */
int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC) {
        ts->tv_sec = 2000000000;
        ts->tv_nsec = 0;
        return 0;
    }
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    return 0;
}

static const char *kv[16];          /* key, value, ..., NULL */

int settings_get(const char *key, char *val, size_t len)
{
    for (size_t i = 0; kv[i]; i += 2)
        if (!strcmp(kv[i], key)) {
            snprintf(val, len, "%s", kv[i + 1]);
            return 0;
        }
    return -1;
}

/* One hold file, as the extension host writes it. */
static void ext_hold_file(const char *id, int required, int raised, const char *reason, double ts)
{
    char path[160];
    snprintf(path, sizeof(path), "cool-engine-test/holds/%s.json", id);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "{\"id\": \"%s\", \"required\": %s, \"raised\": %s, \"reason\": \"%s\", \"ts_mono\": %.3f}\n", id,
            required ? "true" : "false", raised ? "true" : "false", reason, ts);
    fclose(f);
}

static char last_log[256];
static char logbuf[16384];          /* every line since an intake case began */

void fflog(int prio, const char *fmt, ...)
{
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    size_t n = strlen(logbuf);
    if (n + strlen(last_log) + 2 < sizeof(logbuf))
        snprintf(logbuf + n, sizeof(logbuf) - n, "%s\n", last_log);
    printf("    log: %s\n", last_log);
}

static int log_has(const char *needle)
{
    return strstr(logbuf, needle) != NULL;
}

int diag_running(void) { return 0; }
int status_json(char *buf, size_t len) { (void)buf; (void)len; return 0; }

/* The sensors logger's hook, captured: the flight recorder's arming
   counted. */
static int events;
void senslog_event(const char *msg) { (void)msg; events++; }

/* The faked crash watch hardware: armed on request, and the next poll
   reports what the test put in src1/src2, or fails (a head that stopped
   answering). */
static int crash_armed_fake;
static int crash_arm_calls;
static int crash_poll_fail;
static unsigned crash_src1, crash_src2;
int fake_crash_open(void) { return 0; }
int fake_crash_arm(int x, int y, int abort)
{
    (void)x; (void)y; (void)abort;
    crash_armed_fake = 1;
    crash_arm_calls++;
    return 0;
}
int fake_crash_poll(unsigned *s1, unsigned *s2)
{
    if (crash_poll_fail)
        return -1;
    *s1 = crash_src1;
    *s2 = crash_src2;
    return 0;
}
void fake_crash_disarm(void) { crash_armed_fake = 0; }
void fake_crash_close(void) { }

/* The supervisor's stop, counted: what the kernel attributes read at
   the moment of the call is the proof the writes came first. */
static int stops;
static char stop_why[64];
static long stop_seen_stop, stop_seen_latch;
static long get_long(const char *attr);
static void fake_stop(const char *why)
{
    stops++;
    snprintf(stop_why, sizeof(stop_why), "%s", why);
    stop_seen_stop = get_long("cnc/stop");
    stop_seen_latch = get_long("cnc/laser_latch");
}
static int machine_idle = 1;        /* the kernel idle, as status.c reads it */
int machine_is_idle(void) { return machine_idle; }
int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    (void)t; (void)a; (void)fn; (void)arg;
    return 0;
}
static char last_flag[160];
int setup_flag(const char *id, const char *reason)
{
    snprintf(last_flag, sizeof(last_flag), "%s:%s", id, reason);
    return 0;
}
/* The coolant flow watch, faked: its yields counted with the heater as each
   one found it (a yield lands before the engine's own heater write), with the
   quiet hold, the diagnostic's take and the pump as each one found them, and
   the last tick's context; its stop counted with the heater as it found it. */
static int flow_yields;
static long flow_yield_heater = -1, flow_yield_pump = -1;
static int flow_yield_quiet, flow_yield_diag;
static int flow_stops;
static long flow_stop_heater = -1;
static flowrun_ctx_t flow_ctx;
void flowrun_init(void) { }
void flowrun_shutdown(void) { flow_stops++; flow_stop_heater = get_long("thermal/heater_pwm"); }
void flowrun_engine(const flowrun_ctx_t *c) { flow_ctx = *c; }
void flowrun_yield(const char *why)
{
    (void)why;
    flow_yields++;
    flow_yield_heater = get_long("thermal/heater_pwm");
    flow_yield_pump = get_long("thermal/water_pump_on");
    flow_yield_quiet = quiet_held && quiet_pump_held;
    flow_yield_diag = diag_owned;
}
/* The button as the engine sees it, and the supervisor's motion check. */
static int led_red, led_released, led_writes, motion_fault_stub;
static int led_unreadable;              /* the LED cannot be read */
void led_set(led_pattern_t p) { led_red = p == LED_FLASH_RED; led_released = 0; led_writes++; }
void led_release(void) { led_red = 0; led_released = 1; led_writes++; }
int led_red_up(void) { return led_unreadable ? -1 : led_red; }
int super_motion_fault(void) { return motion_fault_stub; }
double chassis_degc(void) { return 25.0; }
long supply_temp_raw(void) { return 540; }
double soc_degc(void) { return 38.0; }
static long throttle_stub;              /* the kernel's CPU cooling state */
long soc_throttle_state(void) { return throttle_stub; }

/* The factory conversion (status.c), and its inverse for the model. */
double coolant_degc(long raw)
{
    static const double adc_f = 1024.0 * 1.3;
    if (raw <= 0 || (double)raw >= adc_f)
        return -273.15;
    double r = 10000.0 / (adc_f / (double)raw - 1.0);
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    return 3380.0 / log(r / rinf) - 273.15;
}

static long raw_for(double c)
{
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    double r = rinf * exp(3380.0 / (c + 273.15));
    return (long)(1024.0 * 1.3 / (1.0 + 10000.0 / r) + 0.5);
}

/* --- the fake tree ---------------------------------------------------- */

static void put(const char *attr, const char *v)
{
    char path[160];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(v, f);
        fclose(f);
    }
}

static void put_long(const char *attr, long v)
{
    char s[24];
    snprintf(s, sizeof(s), "%ld\n", v);
    put(attr, s);
}

static long get_long(const char *attr)
{
    char path[160], buf[24];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v = fgets(buf, sizeof(buf), f) ? strtol(buf, NULL, 10) : -1;
    fclose(f);
    return v;
}

static void make_tree(void)
{
    mkdir("cool-engine-test", 0755);
    mkdir("cool-engine-test/run", 0755);
    mkdir(GF_SYSFS, 0755);
    mkdir(GF_SYSFS "pic", 0755);
    mkdir(GF_SYSFS "thermal", 0755);
    mkdir(GF_SYSFS "head", 0755);
    mkdir(GF_SYSFS "cnc", 0755);
    put("cnc/state", "idle\n");
    put_long("cnc/faults", 0);
    put_long("cnc/laser_on_sampled", 0);
    put_long("cnc/laser_pgood_sampled", 0);
    /* Tach counts are periods: a small count is a fast fan. */
    put_long("thermal/tach_exhaust", 300);
    put_long("thermal/tach_intake_1", 300);
    put_long("thermal/tach_intake_2", 300);
    put_long("head/air_assist_tach", 300);
    put_long("head/purge_air_current", 629);
    put_long("thermal/heater_pwm", 0);
    put_long("pic/hv_current", 0);
    /* The fan duties the engine writes and reads back. */
    put_long("head/air_assist_pwm", 204);
    put_long("thermal/exhaust_pwm", 0);
    put_long("thermal/intake_pwm", 0);
    put_long("thermal/tec_on", 0);
}

/* --- the loop model --------------------------------------------------- */

static const double BASE_C = 22.3;

typedef struct {
    int dead_down, dead_up;         /* a sensor whose read fails */
    int session;                    /* ticks since the session opened */
    double bulk_c;                  /* the bulk warmed past its base (a warm-up) */
    double down_extra_c;            /* downstream over upstream (the loop heater's own heat) */
} loop_t;

static void loop_tick(loop_t *L)
{
    /* The run airflow's ADC offset: in from the session open, toggling. */
    double offset = L->session > 0 ? -1.0 + ((L->session & 1) ? 0.6 : 0.0) : 0.0;
    double down = BASE_C + L->bulk_c + offset + L->down_extra_c;
    double up = BASE_C + L->bulk_c + offset;
    if (L->dead_down)
        unlink(GF_SYSFS "pic/water_temp_1");
    else
        put_long("pic/water_temp_1", raw_for(down));
    if (L->dead_up)
        unlink(GF_SYSFS "pic/water_temp_2");
    else
        put_long("pic/water_temp_2", raw_for(up));
    if (L->session > 0)
        L->session++;
}

/* --- driving the engine ----------------------------------------------- */

static int attn_keep;

/* One tick of an idle machine. */
static void run_idle_tick(loop_t *L)
{
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("idle", 0, -1, -1, -1, NULL);
    engine_tick();
}

/* Close the session the way a job's end does: report idle and tick
   through the cooldown phases until the engine is idle, then take the
   heater off. */
static void close_session(loop_t *L)
{
    L->session = 0;
    for (int i = 0; i < 900; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
        if (cool_state == Cool_Idle)
            break;
    }
    heater_set_pct(0);
    conf_reload();
    /* Only the attention case wants a failure left standing for the next job. */
    if (!attn_keep)
        (void)cool_attention_ack();
}

/* One tick of an open run session. */
static void run_tick(loop_t *L)
{
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
}

/* Settle idle and open a run session (the session's first tick runs). The
 * engine reads its settings at the run start, so a case that sets them
 * after close_session sees them take effect here. */
static void settle_and_open(loop_t *L)
{
    for (int i = 0; i < 20; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
    }
    L->session = 1;
    run_tick(L);
}

/* Open a run session on a settled loop. */
static void open_session(loop_t *L)
{
    close_session(L);
    settle_and_open(L);
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* The armed acknowledgment waits for the fans: with the exhaust under
 * its floor the verdict carries armed=false however long the window is
 * open; the tick the exhaust reads its floor, the acknowledgment goes
 * out and the engine says so. A fan that never comes up never
 * acknowledges (the controller refuses the arm at its budget). */
static void test_ack_waits_for_fans(loop_t *L)
{
    printf("L. the armed acknowledgment waits for the fans\n");
    close_session(L);
    put_long("thermal/tach_exhaust", 10000000);     /* 3000 rpm: under the 6400 floor */
    for (int i = 0; i < 20; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
    }
    L->session = 1;
    int acked_early = 0;
    for (int i = 0; i < 25; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("run", 1, -1, -1, -1, NULL);
        engine_tick();
        if (pub_armed_ack)
            acked_early = 1;
    }
    CHECK(!acked_early, "no acknowledgment while the exhaust is under its floor (25 s)");
    CHECK(!fans_up, "fans_up is false with the exhaust slow");
    put_long("thermal/tach_exhaust", 300);
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
    CHECK(pub_armed_ack && fans_up, "the acknowledgment goes out the tick the exhaust reads its floor");
    put_long("thermal/tach_exhaust", 300);
    close_session(L);
}

/* --- the fan writes and the intakes fitted ------------------------------ */

/* Tach periods. 300 is a fast fan; 3518 is the air assist at its idle
 * duty on the bench reference (about 2130 rpm, under the 6093 floor). In
 * ns at 2 pulses/rev, 7200000 is about 4170 rpm, an intake at its run
 * duty on the bench reference; 0 is a tach that saw no pulse, which is
 * what the kernel reads with no fan on it. */
#define AA_FAST     300
#define AA_IDLE     3518
#define TACH_RUN    7200000
#define TACH_NONE   0

/* The tree these cases start from: a settled loop at 22.3 C, the supply
 * good, every fan at its idle duty and fast but the intakes named. */
static void fan_tree(long intake_1, long intake_2)
{
    put_long("cnc/laser_pgood_sampled", 255);
    put_long("thermal/tach_exhaust", 300);
    put_long("thermal/tach_intake_1", intake_1);
    put_long("thermal/tach_intake_2", intake_2);
    put_long("head/air_assist_tach", AA_FAST);
    put_long("pic/hv_current", 0);
    put_long("pic/water_temp_1", raw_for(22.3));
    put_long("pic/water_temp_2", raw_for(22.3));
    put_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    put_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    put_long("thermal/intake_pwm", INTAKE_IDLE);
}

static void tick(const char *mode, int armed)
{
    fake_now += 1.0;
    cool_state_report(mode, armed, -1, -1, -1, NULL);
    engine_tick();
}

/* Close any session and settle idle, the fake device honest again. */
static void fan_settle(void)
{
    drop_attr = NULL;
    drop_left = 0;
    refuse = 0;
    put_long("head/air_assist_tach", AA_FAST);
    for (int i = 0; i < 400; i++) {
        tick("idle", 0);
        if (cool_state == Cool_Idle)
            break;
    }
}

static void fw_clean(void)
{
    printf("fan writes A. a session opens and every fan reads its run duty\n");
    fan_settle();
    tick("run", 1);
    CHECK(cool_state == Cool_Run, "the session is open");
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "air assist at run duty");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN, "exhaust at run duty");
    CHECK(get_long("thermal/intake_pwm") == INTAKE_RUN, "intake at run duty");
    CHECK(fan_cmd[Fx_AirAssist] == AIR_ASSIST_RUN, "the command mirror agrees");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(fan_lost[Fx_AirAssist] == 0 && fan_lost[Fx_Exhaust] == 0,
          "no fan lost its duty through the grace and after");
    CHECK(!airflow_alarm, "no airflow fault");
}

static void fw_dropped(void)
{
    printf("fan writes B. two writes dropped, the third takes\n");
    fan_settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = 2;
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "the third try took");
    CHECK(drop_left == 0, "both dropped writes were spent before the third took");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no airflow fault");
}

static void fw_lost(void)
{
    printf("fan writes C. a device loses its duty mid-session\n");
    fan_settle();
    tick("run", 1);
    for (int i = 0; i < 20; i++)
        tick("run", 1);                 /* past the spin-up grace */
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "at run duty before the loss");
    /* The head resets: its register back at idle, the fan winding down. */
    put_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "the tick put the duty back");
    CHECK(fan_lost[Fx_AirAssist] == 1, "counted once");
    /* The fan spins up over a few seconds; the grace covers it. */
    for (int i = 0; i < 5; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no fault while the fan spins back up inside the grace");
    put_long("head/air_assist_tach", AA_FAST);
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no fault once the fan is back at speed");
}

static void fw_refused(void)
{
    printf("fan writes D. a device takes nothing: the fault names the duty and the readback\n");
    fan_settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = -1;
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_IDLE, "the head kept its idle duty");
    for (int i = 0; i < 25 && !airflow_alarm; i++)
        tick("run", 1);
    CHECK(airflow_alarm, "the fan is tripped at its tach");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: air_assist") != NULL, "the airflow fault");
    CHECK(strstr(pub_reason, "(duty 1023, reads 204)") != NULL,
          "the fault names the duty commanded and the duty in force");
    CHECK(strstr(pub_reason, "hold, no resume this job") != NULL, "the hold stands");
}

static void fw_refused_bus(void)
{
    printf("fan writes D2. the bus refuses the write: named, and the fault the same\n");
    fan_settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = -1;
    refuse = 1;
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    for (int i = 0; i < 25 && !airflow_alarm; i++)
        tick("run", 1);
    CHECK(airflow_alarm && strstr(pub_reason, "(duty 1023, reads 204)") != NULL,
          "the fault names the duty in force");
}

static void fw_thermal_lost(void)
{
    printf("fan writes E. the exhaust PWM loses its duty: the same route puts it back\n");
    fan_settle();
    tick("run", 1);
    for (int i = 0; i < 20; i++)
        tick("run", 1);
    put_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    tick("run", 1);
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN, "the exhaust duty is put back");
    CHECK(fan_lost[Fx_Exhaust] == 1 && fan_lost[Fx_AirAssist] == 0, "counted on the right fan");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no fault");
}

/* Close any session and settle idle; the next session reloads the
 * settings a case sets. */
static void intake_settle(void)
{
    for (int i = 0; i < 400; i++) {
        tick("idle", 0);
        if (cool_state == Cool_Idle)
            break;
    }
    put_long("thermal/tach_intake_1", TACH_RUN);
    put_long("thermal/tach_intake_2", TACH_NONE);
    kv[0] = NULL;
    logbuf[0] = '\0';
}

/* An armed session, run until it faults or for `ticks` ticks. */
static void armed_session(int ticks)
{
    for (int i = 0; i < ticks && !airflow_alarm; i++)
        tick("run", 1);
}

/* The published row for one fan: its reading, floor and state. */
static int gate_is(const char *id, double reading, double floor, const char *state)
{
    json_error_t e;
    json_t *j = json_loads(pub_fan_gates, 0, &e);
    json_t *g = json_object_get(j, id);
    const char *s = json_string_value(json_object_get(g, "state"));
    int ok = g && json_number_value(json_object_get(g, "reading")) == reading &&
             json_number_value(json_object_get(g, "floor")) == floor && s && !strcmp(s, state);
    json_decref(j);
    return ok;
}

static void in_two_fans(void)
{
    printf("intakes A. two fans, the default: the silent intake 2 is judged\n");
    intake_settle();
    armed_session(40);
    CHECK(intake_fans == 2, "the default is two intake fans");
    CHECK(airflow_alarm, "the silent tach trips the airflow gate");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: intake_2 0 under") != NULL, "the fault names intake_2");
    CHECK(strstr(pub_reason, "hold, no resume this job") != NULL, "the hold stands");
    CHECK(!fans_up && !pub_armed_ack, "the armed window was never acknowledged");
    CHECK(gate_is("intake_2", 0, 2290, "TRIPPED"), "intake_2 is published tripped at the intake floor");
}

static void in_one_fan(void)
{
    printf("intakes B. one fan: intake 2 is not judged, intake 1 is\n");
    intake_settle();
    kv[0] = "cool_intake_fans"; kv[1] = "1"; kv[2] = NULL;
    armed_session(40);
    CHECK(intake_fans == 1, "the setting is in force");
    CHECK(!airflow_alarm, "no airflow fault through the grace and after");
    CHECK(fans_up && pub_armed_ack, "the armed window is acknowledged");
    printf("    fan gates: %s\n", pub_fan_gates);
    CHECK(gate_is("intake_2", 0, 0, "not fitted"), "intake_2 is published not fitted, with no floor");
    CHECK(gate_is("intake_1", 4167, 2290, "ok"), "intake_1 is judged at the intake floor");
}

static void in_one_fan_intake1_silent(void)
{
    printf("intakes C. one fan, and intake 1 stops: intake 1 still trips\n");
    intake_settle();
    kv[0] = "cool_intake_fans"; kv[1] = "1"; kv[2] = NULL;
    put_long("thermal/tach_intake_1", TACH_NONE);
    armed_session(40);
    CHECK(airflow_alarm, "the airflow gate trips");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: intake_1 0 under the 2290 floor") != NULL,
          "the fault names intake_1 at the intake floor");
    CHECK(!fans_up, "the armed window was never acknowledged");
}

static void in_one_fan_gate_off(void)
{
    printf("intakes D. one fan, the intake gate off: intake_2 is not judged, intake_1 still is\n");
    intake_settle();
    kv[0] = "cool_intake_fans"; kv[1] = "1";
    kv[2] = "cool_tach_intake_min_rpm"; kv[3] = "0"; kv[4] = NULL;
    armed_session(40);
    CHECK(!airflow_alarm, "no airflow fault");
    CHECK(!fan_would_warned, "the silent intake_2 drew no would-have-tripped warning: it is not judged");
    CHECK(gate_is("intake_2", 0, 0, "not fitted"), "intake_2 is still published not fitted");
    /* The control: the off gate still names a fitted fan under the
     * default, so the quiet above is the judging's, not the warning's. */
    put_long("thermal/tach_intake_1", TACH_NONE);
    armed_session(40);
    CHECK(!airflow_alarm && fan_would_warned, "intake_1 under the default draws the warning, and no fault");
    CHECK(gate_is("intake_1", 0, 0, "off"), "intake_1 is published off at its reading");
}

static void in_other_value(void)
{
    printf("intakes E. a hand-edited 0: intake 2 stays judged\n");
    intake_settle();
    kv[0] = "cool_intake_fans"; kv[1] = "0"; kv[2] = NULL;
    armed_session(40);
    CHECK(airflow_alarm && strstr(pub_reason, "AIRFLOW: intake_2") != NULL,
          "only 1 takes intake_2 out of the judging");
}

static void fan_writes(void)
{
    fan_tree(300, 300);
    kv[0] = "cool_tach_air_assist_min_rpm"; kv[1] = "6093"; kv[2] = NULL;
    fw_clean();
    fw_dropped();
    fw_lost();
    fw_refused();
    fw_refused_bus();
    fw_thermal_lost();
    kv[0] = NULL;
}

static void intakes(void)
{
    fan_tree(TACH_RUN, TACH_NONE);
    in_two_fans();
    in_one_fan();
    in_one_fan_intake1_silent();
    in_one_fan_gate_off();
    in_other_value();
    kv[0] = NULL;
}

/* --- the phases' duties, the gates moved past the loop, the TEC ------- */

/* The exhaust at its run duty on the bench reference: about 11640 rpm. */
#define EXHAUST_TACH_RUN 2577320

/* The verdict document as a controller reads it carries this text. */
static int verdict_has(const char *needle)
{
    char buf[512];
    FILE *f = fopen(VERDICT_FILE, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* The published state of one fan's gate. */
static int fan_state_is(const char *id, const char *state)
{
    json_error_t e;
    json_t *j = json_loads(pub_fan_gates, 0, &e);
    const char *s = json_string_value(json_object_get(json_object_get(j, id), "state"));
    int ok = s && !strcmp(s, state);
    json_decref(j);
    return ok;
}

/* The session ends: one idle report, one tick. */
static void session_end_tick(loop_t *L)
{
    L->session = 0;
    loop_tick(L);
    tick("idle", 0);
}

/* A diagnostic's heater is the diagnostic's to keep alive. The kernel holds a
 * duty above zero only for its lease, and the engine's tick writes nothing
 * for a diagnostic, so one that hangs loses its heat with the lease. */
static void diag_heater_lease(loop_t *L)
{
    printf("diagnostics. the engine writes no heater on a diagnostic's behalf\n");
    close_session(L);
    run_idle_tick(L);
    CHECK(cool_diag_take() == 0, "a diagnostic takes the thermal hardware from an idle engine");
    cool_diag_heater_pct(40);
    CHECK(get_long("thermal/heater_pwm") == 40L * 65535 / 100, "its heater write lands");
    /* The lease runs out: the kernel puts the duty to 0, and nothing here
     * may put it back. */
    put_long("thermal/heater_pwm", 0);
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    CHECK(get_long("thermal/heater_pwm") == 0,
          "the lease lapsed: three ticks on, the engine has not written the heater again");
    CHECK(!strcmp(pub_phase, "diag") && !pub_fire_ok && pub_hold, "the phase is diag, fire blocked, hold");
    cool_diag_release();
    run_idle_tick(L);
    CHECK(get_long("thermal/heater_pwm") == 0 && heater_pct == 0 && !strcmp(pub_phase, "idle"),
          "the release leaves the heater off and the engine idle");
}

static void phases(loop_t *L)
{
    printf("phases A. a closed session ends at the idle duties\n");
    open_session(L);
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN && get_long("thermal/intake_pwm") == INTAKE_RUN,
          "the session at its run duties");
    close_session(L);
    CHECK(cool_state == Cool_Idle && get_long("thermal/exhaust_pwm") == 0 && get_long("thermal/intake_pwm") == 0,
          "idle: the exhaust and intake PWMs at 0");

    printf("phases B. a busy start takes the cooldown duties and gives them up once the machine is idle\n");
    machine_idle = 0;
    cool_init();
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_COOL && get_long("thermal/intake_pwm") == INTAKE_COOL,
          "the kernel busy at the start: the cooldown duties");
    for (int i = 0; i < 3; i++)
        tick("idle", 0);
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_COOL, "they stand while the kernel is busy");
    machine_idle = 1;
    logbuf[0] = '\0';
    tick("idle", 0);
    CHECK(get_long("thermal/exhaust_pwm") == 0 && get_long("thermal/intake_pwm") == 0,
          "idle with no session: the idle duties");
    CHECK(log_has("the machine is idle after the busy start"), "and the line says so");
}

static void gate_ceiling(loop_t *L)
{
    printf("gates A. the ceiling under the loop holds the run at its start; at its off end it is skipped\n");
    close_session(L);
    CHECK(!strcmp(pub_verdict, "OK"), "before the run: OK");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && !pub_fire_ok && pub_hold,
          "a ceiling under the loop: OVERTEMP, fire blocked, hold at the run start");
    close_session(L);
    kv[0] = "cool_temp_max"; kv[1] = "60"; kv[2] = NULL;
    logbuf[0] = '\0';
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "OK") && strstr(pub_gates_off, "\"coolant_max\""),
          "the ceiling at 60: OK, coolant_max in gates_off");
    CHECK(log_has("gate coolant_max OFF: cool_temp_max = 60"), "the run-start line names the gate off");
    kv[0] = NULL;
    close_session(L);
}

/* --- the operator's attention ----------------------------------------- */

static void attention(loop_t *L)
{
    char doc[COOL_STATUS_JSON_MAX];
    printf("the operator's attention:\n");
    close_session(L);
    run_idle_tick(L);
    cool_notices_dismiss((unsigned)-1); /* the cases before this one's */
    attn_keep = 1;
    CHECK(!led_red, "a quiet machine: no red");
    settle_and_open(L);
    int was = led_writes;
    run_tick(L);
    run_tick(L);
    CHECK(!led_red && led_writes == was, "an OK session: no red");

    /* A failure: red while it stands, and it cannot be acknowledged away. */
    L->dead_down = 1;
    run_tick(L);
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && led_red, "a blocking failure flashes red");
    CHECK(cool_attention_ack() == -1, "an acknowledgment does not end a failure that stands");
    CHECK(led_red, "the red stays");

    /* A controller's write is overwritten, and a standing flash is left alone. */
    led_red = 0;
    run_tick(L);
    CHECK(led_red, "a controller writing over the flash is overwritten on the next tick");
    was = led_writes;
    run_tick(L);
    run_tick(L);
    CHECK(led_writes == was, "a standing flash is not rewritten");

    /* Its cause gone under a job still held: the job's press is the operator's. */
    L->dead_down = 0;
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && !led_red && led_released,
          "a pause tier's cause gone under the job: no red, nothing to acknowledge");
    cool_status_json(doc, sizeof(doc));
    json_error_t jerr;
    json_t *jdoc = json_loads(doc, 0, &jerr);
    json_t *notices = jdoc ? json_object_get(jdoc, "notices") : NULL;
    json_t *first = notices ? json_array_get(notices, 0) : NULL;
    CHECK(json_array_size(notices) == 1, "and a notice of it stands");
    CHECK(first && !strcmp(json_string_value(json_object_get(first, "verdict")), "SENSOR"),
          "it says the type");
    CHECK(first && json_number_value(json_object_get(first, "at")) > 0.0 &&
          json_number_value(json_object_get(first, "cleared_at")) >=
          json_number_value(json_object_get(first, "at")), "and when it started and cleared");
    json_int_t seq = first ? json_integer_value(json_object_get(first, "seq")) : 0;
    if (jdoc)
        json_decref(jdoc);

    /* The same again counts on it; another stacks; the oldest goes past the cap. */
    L->dead_down = 1;
    run_tick(L);
    run_tick(L);
    run_tick(L);
    L->dead_down = 0;
    run_tick(L);
    run_tick(L);
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"times\":2") != NULL, "a repeat counts on the notice");
    cool_notices_dismiss((unsigned)seq - 1);
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"times\":2") != NULL, "a dismissal of older ones leaves it");
    cool_notices_dismiss((unsigned)seq);
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"notices\":[]") != NULL, "the dismissal takes it");
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "and it never blocked anything");

    /* The same failure standing when the session ends stays up. */
    L->dead_down = 1;
    run_tick(L);
    run_tick(L);
    run_tick(L);
    close_session(L);
    L->dead_down = 0;
    run_idle_tick(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && led_red, "a failure that outlived its session waits for the operator");
    CHECK(cool_attention_ack() == 0, "the acknowledgment takes it");
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !led_red, "and the machine is free");

    /* The same failure standing when the armed window closes, its cause gone
     * while the engine is still in the session's tail: the window closing
     * over it is enough to make it the operator's. */
    settle_and_open(L);
    run_tick(L);
    L->dead_down = 1;
    run_tick(L);
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && attn_latched && !attn_sticky, "the failure stands in the armed window");
    session_end_tick(L);
    CHECK(cool_state == Cool_Smoke && !strcmp(pub_verdict, "SENSOR"), "the window closes over it: the smoke phase, SENSOR still");
    L->dead_down = 0;
    for (int i = 0; i < 2; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    cool_status_json(doc, sizeof(doc));
    CHECK(cool_state == Cool_Smoke && !strcmp(pub_verdict, "ACK") && !pub_fire_ok && led_red &&
          strstr(doc, "\"notices\":[]") != NULL,
          "its cause gone during the smoke phase: ACK all the same, fire blocked, red, no notice");
    CHECK(cool_attention_ack() == 0, "the acknowledgment takes it");
    close_session(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !led_red, "and the machine is free");

    /* A job-ending failure: the session's end clears its verdict, and it blocks the next job. */
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_temp_critical_c"; kv[5] = "20"; kv[6] = NULL;
    settle_and_open(L);
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "CRITICAL") && led_red, "the fail tier flashes red");
    CHECK(attn_latched && attn_sticky, "CRITICAL is the fail tier: it takes the acknowledgment, its window still open");
    CHECK(verdict_has("\"ack_wait\":false"), "while its cause stands nothing waits for the acknowledgment yet");
    kv[0] = NULL;
    close_session(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && !pub_fire_ok, "its session over: ACK, fire blocked");
    CHECK(strstr(pub_reason, "CRITICAL") && !strcmp(pub_reason, attn_reason) &&
          verdict_has("\"reason\":\"CRITICAL"), "ACK carries the fault's words, in the verdict file too");
    CHECK(verdict_has("\"ack_wait\":true"), "and the verdict file's ack_wait says so");
    CHECK(!pub_hold, "and motion free");
    CHECK(led_red, "the red stays");
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"standing\":true,\"live\":false,\"verdict\":\"CRITICAL\"") != NULL,
          "the panel sees it standing, not live, by name");
    settle_and_open(L);
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "ACK"), "an armed window under it: fire blocked and held");
    close_session(L);
    run_idle_tick(L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "ACK"), "and it still stands after that session");
    CHECK(cool_attention_ack() == 0, "the acknowledgment takes it");
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && !led_red && led_released, "OK, fire free, the red out");
    CHECK(verdict_has("\"ack_wait\":false"), "and ack_wait is false again");
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"notices\":[]") != NULL, "an acknowledged failure leaves no notice");
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"standing\":false") != NULL, "the panel sees nothing standing");

    /* A hold the machine waits out on its own is not the operator's. */
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    settle_and_open(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && !led_red, "a hot loop is no red");
    kv[0] = NULL;
    close_session(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !led_red, "and leaves nothing to acknowledge");
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"notices\":[]") != NULL, "and no notice");

    /* A fire: it ends with its session's tail, and the acknowledgment carries
     * it (fire blocked, motion free) until the panel's Acknowledge. */
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    settle_and_open(L);
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 900);               /* past the critical quartiles */
    }
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FIRE") && led_red, "a fire: FIRE, red");
    CHECK(attn_latched && attn_sticky, "FIRE is the fail tier: it takes the acknowledgment, its window still open");
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    close_session(L);
    run_idle_tick(L);
    CHECK(!fire_alarm && !strcmp(pub_verdict, "ACK") && !pub_fire_ok && !pub_hold && led_red,
          "the session's tail over: the fire alarm gone, ACK (fire blocked, motion free), red");
    CHECK(cool_attention_ack() == 0, "the acknowledgment takes it");
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "and the machine can fire again");

    /* A flame that starts in the smoke phase, after the window closed: the
     * watch standing down at idle is not the flame clearing. */
    settle_and_open(L);
    run_tick(L);
    L->session = 0;
    int flamed = 0;
    for (int i = 0; i < 900 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
        if (i == 1)
            for (int k = 1; k <= 4; k++) {
                char a[24];
                snprintf(a, sizeof(a), "pic/lid_ir_%d", k);
                put_long(a, 400);       /* over the alert, under the critical */
            }
        if (!strcmp(pub_verdict, "FLAME"))
            flamed = 1;
    }
    run_idle_tick(L);
    cool_status_json(doc, sizeof(doc));
    CHECK(flamed && !strcmp(pub_verdict, "ACK") && led_red && strstr(doc, "\"notices\":[]"),
          "a flame still burning when the engine returns to idle needs the Acknowledge, no notice");
    CHECK(cool_attention_ack() == 0, "the acknowledgment takes it");
    for (int i = 1; i <= 4; i++) {
        char a[64];
        snprintf(a, sizeof(a), GF_SYSFS "pic/lid_ir_%d", i);
        unlink(a);
    }
    heater_set_pct(0);
    conf_reload();
    run_idle_tick(L);

    /* A failed motion check: red for as long as it stands. */
    motion_fault_stub = 1;
    run_idle_tick(L);
    CHECK(led_red, "a failed motion check flashes red");
    motion_fault_stub = 0;
    run_idle_tick(L);
    CHECK(!led_red, "and stops when it is cleared");

    /* The engine puts out only its own flash. A button another owner wrote
     * since the last tick (the supervisor's amber, a wizard's teal) no longer
     * reads as the flash, and is left as that owner set it. */
    motion_fault_stub = 1;
    run_idle_tick(L);
    CHECK(led_red, "the flash is up again");
    motion_fault_stub = 0;
    led_red = 0;                        /* another owner's pattern landed over it */
    led_released = 0;
    int writes = led_writes;
    run_idle_tick(L);
    CHECK(led_writes == writes && !led_released, "a button another owner wrote since is not blanked");
    run_idle_tick(L);
    CHECK(led_writes == writes, "and the engine does not come back for it");
    /* An LED that cannot be read is put out all the same: a red left
     * flashing would say firing is blocked when it is not. */
    motion_fault_stub = 1;
    run_idle_tick(L);
    motion_fault_stub = 0;
    led_unreadable = 1;
    led_released = 0;
    run_idle_tick(L);
    led_unreadable = 0;
    CHECK(led_released, "a flash that cannot be read back is put out");
    attn_keep = 0;
}

/* A lid IR read that fails under a standing FLAME: the hold stands, nothing
 * is said to have cleared, and the blind tick is no part of the five clean
 * readings in a row that release it. */
static void flame_blind_tick(loop_t *L)
{
    printf("O3. a lid IR read that fails under a standing FLAME releases nothing\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    settle_and_open(L);
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 300);               /* q1 past its alert, under its critical */
    }
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FLAME") && pub_hold && !pub_fire_ok, "FLAME stands: hold, fire blocked");
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);               /* the signal gone */
    }
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FLAME"), "two clean readings: still FLAME");
    unlink(GF_SYSFS "pic/lid_ir_3");    /* one channel's read fails */
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FLAME") && !pub_fire_ok && pub_hold && verdict_has("\"resume_ok\":false"),
          "a failed lid IR read: still FLAME, fire blocked, hold, no resume");
    CHECK(flame_alert && attn_latched && attn_live && attn_notice_n == 0,
          "the hold and the attention stand, and no notice says it cleared");
    put_long("pic/lid_ir_3", 100);
    for (int i = 0; i < FIRE_CLEAR_TICKS - 1; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "FLAME") && !pub_fire_ok,
          "four clean readings after it: still FLAME (the blind tick was not a clean one, and the count began again)");
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && !pub_hold && attn_notice_n == 1,
          "the fifth clean reading in a row releases it, and the notice is filed then");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
}

static void gate_warmup(loop_t *L)
{
    printf("gates B. the start gate over the loop holds a warm-up; the bulk, not a slug, releases it\n");
    close_session(L);
    kv[0] = "cool_temp_start"; kv[1] = "23.3"; kv[2] = NULL;     /* 1 C over the loop's 22.3 */
    settle_and_open(L);
    CHECK(cool_state == Cool_Warmup && !strcmp(pub_verdict, "WARMUP") && pub_hold && !pub_fire_ok &&
          !strcmp(pub_phase, "warm-up"), "WARMUP, hold, fire blocked, phase warm-up");
    CHECK(get_long("thermal/heater_pwm") > 0, "the loop heater on");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_IDLE && get_long("thermal/intake_pwm") == INTAKE_IDLE &&
          get_long("head/air_assist_pwm") == AIR_ASSIST_IDLE, "the fans at their idle duties");
    for (int i = 0; i < WARMUP_WIN + 10; i++)
        run_tick(L);
    L->bulk_c = 3.0;                    /* one warm sample: a slug passing the sensor */
    run_tick(L);
    L->bulk_c = 0.0;
    run_tick(L);
    CHECK(cool_state == Cool_Warmup, "one warm sample does not release it");
    L->bulk_c = 2.5;                    /* the bulk warms past the gate */
    for (int i = 0; i < WARMUP_WIN - 1; i++)
        run_tick(L);
    CHECK(cool_state == Cool_Warmup, "the minute's minimum still under the gate: held");
    logbuf[0] = '\0';
    run_tick(L);
    CHECK(cool_state == Cool_Run && get_long("thermal/heater_pwm") == 0,
          "the minute's minimum at the gate: released into run, the heater 0");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN && get_long("thermal/intake_pwm") == INTAKE_RUN,
          "the run duties");
    CHECK(log_has("warm-up complete"), "the line says the warm-up is complete");
    close_session(L);
    L->bulk_c = 0.0;
    kv[0] = "cool_temp_min"; kv[1] = "0"; kv[2] = "cool_temp_start"; kv[3] = "0"; kv[4] = NULL;
    logbuf[0] = '\0';
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "OK") && strstr(pub_gates_off, "\"coolant_min\"") &&
          strstr(pub_gates_off, "\"warm_up\""), "both low gates at 0: OK, coolant_min and warm_up in gates_off");
    CHECK(log_has("gate coolant_min OFF: cool_temp_min = 0") && log_has("gate warm_up OFF: cool_temp_start = 0"),
          "the run-start lines name both gates off");
    kv[0] = NULL;
    close_session(L);
}

/* Under a diagnostic the engine senses nothing, and the operator's attention
 * stands as it was: the red flash kept, an acknowledgment that waits still
 * named, a cause that was live not called cleared. Fire stays blocked and the
 * hold stands throughout. */
static void diag_keeps_attention(loop_t *L)
{
    printf("diagnostics. the operator's attention keeps its lamp and its name under a diagnostic\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    attn_keep = 1;
    settle_and_open(L);
    L->dead_down = 1;
    run_tick(L);
    run_tick(L);
    run_tick(L);
    close_session(L);
    L->dead_down = 0;
    run_idle_tick(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && led_red, "a failure waits for its acknowledgment: ACK, red");
    CHECK(cool_diag_take() == 0, "a diagnostic takes the thermal hardware");
    led_red = 0;                        /* the diagnostic's side wrote the button */
    run_idle_tick(L);
    CHECK(!strcmp(pub_phase, "diag") && !strcmp(pub_verdict, "ACK") && verdict_has("\"verdict\":\"ACK\""),
          "under the diagnostic the verdict still reads ACK");
    CHECK(!pub_fire_ok && pub_hold, "fire blocked and the hold standing, as under any diagnostic");
    CHECK(led_red, "and the red flash is put back over what was written");
    CHECK(cool_attention_ack() == 0, "the Acknowledge is taken under the diagnostic");
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !led_red && led_released, "then the name is OK and the red is out");
    CHECK(!pub_fire_ok && pub_hold, "with fire still blocked and the hold standing: the diagnostic's");
    cool_diag_release();
    run_idle_tick(L);

    /* A cause live when the diagnostic took over is not called cleared. */
    L->dead_down = 1;
    run_idle_tick(L);
    run_idle_tick(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && attn_live && !attn_sticky && led_red, "a dead sensor at idle: SENSOR, red");
    CHECK(cool_diag_take() == 0, "a diagnostic takes over under it");
    run_idle_tick(L);
    run_idle_tick(L);
    CHECK(attn_latched && attn_live && attn_notice_n == 0 && led_red,
          "the cause still counts as live: red, and no notice says it cleared");
    CHECK(cool_attention_ack() == -1, "and it cannot be acknowledged away");
    cool_diag_release();
    L->dead_down = 0;
    run_idle_tick(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !led_red && attn_notice_n == 1,
          "the sensor back after the diagnostic: released then, with its notice");
    cool_notices_dismiss((unsigned)-1);
    attn_keep = 0;
}

static void gate_critical(loop_t *L)
{
    printf("gates C. the critical line and the ceiling under the loop: CRITICAL, ended with the session\n");
    close_session(L);
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_temp_critical_c"; kv[5] = "20"; kv[6] = NULL;
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "CRITICAL") && !pub_fire_ok && pub_hold && verdict_has("\"resume_ok\":false"),
          "CRITICAL, fire blocked, hold, no resume");
    CHECK(strstr(pub_reason, "CRITICAL") && strstr(pub_reason, "coolant"),
          "the reason names the tier and the coolant");
    session_end_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP"), "the session over: OVERTEMP, the ceiling still under the loop");
    /* CRITICAL left its acknowledgment waiting, and another verdict has the
     * name: the field carries it where the name cannot. */
    CHECK(verdict_has("\"verdict\":\"OVERTEMP\"") && verdict_has("\"ack_wait\":true") &&
          verdict_has("\"fire_ok\":false"),
          "the acknowledgment CRITICAL left waits under that name: ack_wait true, fire blocked");
    {
        /* The Acknowledge takes CRITICAL's words only: the ceiling's stand. */
        char ceiling[COOL_REASON_MAX];
        snprintf(ceiling, sizeof(ceiling), "%s", pub_reason);
        CHECK(ceiling[0] && strcmp(ceiling, attn_reason) && cool_attention_ack() == 0 &&
              !strcmp(pub_reason, ceiling), "the Acknowledge clears its own fault's words, and leaves the ceiling's");
    }
    /* The ceiling's own hold stands under the case above: with the
     * ceiling at its off end the critical line holds alone. */
    close_session(L);
    kv[1] = "60";
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "CRITICAL") && !pub_fire_ok && pub_hold && verdict_has("\"resume_ok\":false"),
          "the ceiling off: CRITICAL holds alone, no resume");
    close_session(L);
    kv[1] = "18";
    kv[5] = "70";
    logbuf[0] = '\0';
    settle_and_open(L);
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && strstr(pub_gates_off, "\"coolant_critical\""),
          "the critical line at 70: coolant_critical in gates_off, OVERTEMP alone");
    CHECK(log_has("gate coolant_critical OFF: cool_temp_critical_c = 70"), "the run-start line names the gate off");
    kv[0] = NULL;
    close_session(L);
}

/* --- the flow watch's context ------------------------------------------ */

/* The engine's reasons to keep the loop heater from the flow watch, one per
 * term of the context's yield. */
enum { Y_Warmup, Y_Sensor, Y_Quiet, Y_Overtemp, Y_Cold, Y_Critical, Y_Tec, Y_HotUngated, Y_Cooling, Y_N };

/* Which of them stand now, from the engine's own state; returns how many. */
static int yield_terms(int term[Y_N])
{
    term[Y_Warmup] = cool_state == Cool_Warmup;
    term[Y_Sensor] = sensor_alarm;
    term[Y_Quiet] = quiet_held;
    term[Y_Overtemp] = over_temp_gate;
    term[Y_Cold] = cold_gate;
    term[Y_Critical] = critical_alarm;
    term[Y_Tec] = tec_on_state;
    term[Y_HotUngated] = gate_coolant_off && pub_up > TEMP_MAX_C_DEFAULT;
    term[Y_Cooling] = cool_state == Cool_Thermal || forced_cool;
    int n = 0;
    for (int i = 0; i < Y_N; i++)
        n += term[i] != 0;
    return n;
}

/* One row per cause: the settings its session opens with, how far over its
 * 22.3 C the loop goes once the session is open, what the context says, and
 * the fans and the run session it hands the watch. Each row leaves its cause
 * the only one standing, so a term dropped from the engine's yield fails
 * that row and no other. */
static const struct {
    int cause;
    const char *name, *why;
    const char *kv[7];
    double bulk_c;
    int fans, session;
} yield_rows[] = {
    { Y_Warmup,     "a warm-up",                    "warm-up",
      { "cool_temp_start", "30", NULL }, 0.0, FW_FANS_IDLE, 0 },
    { Y_Sensor,     "a coolant sensor unreadable",  "a coolant sensor is unreadable",
      { NULL }, 0.0, FW_FANS_RUN, 1 },
    { Y_Quiet,      "a quiet hold",                 "a quiet hold",
      { NULL }, 0.0, FW_FANS_OTHER, 0 },
    { Y_Overtemp,   "the coolant over the ceiling", "the coolant is over the ceiling",
      { NULL }, 12.0, FW_FANS_RUN, 1 },
    { Y_Cold,       "the coolant under the floor",  "the coolant is under the floor",
      { "cool_temp_min", "30", NULL }, 0.0, FW_FANS_RUN, 1 },
    { Y_Critical,   "the coolant at the critical line", "the coolant is at the critical line",
      { "cool_temp_critical_c", "20", NULL }, 0.0, FW_FANS_RUN, 1 },
    { Y_Tec,        "the TEC running",              "the TEC is cooling",
      { "cool_tec_present", "1", "cool_tec_on_c", "15", "cool_tec_off_c", "13", NULL }, 0.0, FW_FANS_RUN, 1 },
    { Y_HotUngated, "a hot loop with the ceiling gate off", "the coolant is over the ceiling",
      { "cool_temp_max", "60", NULL }, 13.5, FW_FANS_RUN, 1 },
    { Y_Cooling,    "the thermal cooldown",         "the engine is cooling the loop",
      { NULL }, 9.7, FW_FANS_COOL, 0 },
};

static void flow_yields_table(loop_t *L)
{
    char msg[160];
    int term[Y_N];
    printf("K2. the flow watch is kept from the heater by each of the engine's causes, on the tick's own clock\n");
    open_session(L);
    run_tick(L);
    CHECK(yield_terms(term) == 0 && !flow_ctx.yield && fabs(flow_ctx.t - fake_now) < 1e-6,
          "a run session at a normal coolant: no cause stands, and the watch may hold");
    CHECK(flow_ctx.session && flow_ctx.fans == FW_FANS_RUN, "and the watch sees a run session at the run fans");
    CHECK(sizeof(yield_rows) / sizeof(yield_rows[0]) == Y_N, "one row for every cause");
    for (size_t r = 0; r < sizeof(yield_rows) / sizeof(yield_rows[0]); r++) {
        int cause = yield_rows[r].cause;
        close_session(L);
        for (int i = 0; i < 7; i++) {
            kv[i] = yield_rows[r].kv[i];
            if (!kv[i])
                break;
        }
        if (cause == Y_Quiet) {
            run_idle_tick(L);
            cool_quiet_hold(1);
            run_idle_tick(L);
        } else {
            settle_and_open(L);
            L->bulk_c = yield_rows[r].bulk_c;   /* the loop moves once the session is open */
            if (cause == Y_Sensor)
                L->dead_up = 1;
            if (cause == Y_Cooling) {
                run_tick(L);
                session_end_tick(L);
                for (int i = 0; i < 40 && cool_state != Cool_Thermal; i++) {
                    loop_tick(L);
                    tick("idle", 0);
                }
                loop_tick(L);
                tick("idle", 0);
            } else {
                for (int i = 0; i < 40 && !(yield_terms(term), term[cause]); i++)
                    run_tick(L);
                run_tick(L);
            }
        }
        int n = yield_terms(term);
        snprintf(msg, sizeof(msg), "%s: that cause stands, and no other", yield_rows[r].name);
        CHECK(term[cause] && n == 1, msg);
        snprintf(msg, sizeof(msg), "%s: the watch is told to give the heater back, and why (%s)",
                 yield_rows[r].name, yield_rows[r].why);
        CHECK(flow_ctx.yield && flow_ctx.yield_why && !strcmp(flow_ctx.yield_why, yield_rows[r].why), msg);
        snprintf(msg, sizeof(msg), "%s: the context carries the tick's own clock", yield_rows[r].name);
        CHECK(fabs(flow_ctx.t - fake_now) < 1e-6, msg);
        snprintf(msg, sizeof(msg), "%s: the fans and the run session the watch sees", yield_rows[r].name);
        CHECK(flow_ctx.fans == yield_rows[r].fans && flow_ctx.session == yield_rows[r].session, msg);
        if (cause == Y_Quiet)
            cool_quiet_hold(0);
        L->bulk_c = 0.0;
        L->dead_up = 0;
        kv[0] = NULL;
    }
    /* A cause gone gives the heater back to the watch. */
    open_session(L);
    run_tick(L);
    CHECK(yield_terms(term) == 0 && !flow_ctx.yield, "every cause gone: the watch may hold again");
    close_session(L);
    /* A diagnostic builds the context itself: the same yield, the same clock. */
    run_idle_tick(L);
    CHECK(!flow_ctx.session && flow_ctx.fans == FW_FANS_IDLE, "idle: no run session, the idle fans");
    CHECK(cool_diag_take() == 0, "a diagnostic takes the thermal hardware");
    run_idle_tick(L);
    CHECK(flow_ctx.yield && !strcmp(flow_ctx.yield_why, "diagnostics") && fabs(flow_ctx.t - fake_now) < 1e-6,
          "diagnostics: the watch is told to give the heater back, on the tick's own clock");
    CHECK(!flow_ctx.session && flow_ctx.fans == FW_FANS_OTHER, "and sees no run session, the fans not its to judge");
    cool_diag_release();
    run_idle_tick(L);
}

/* A quiet hold and a diagnostic's take each take the heater back from the
 * watch at once, not at the next tick: the yield lands with the hold or the
 * take already published (a tick between them carries it) and, for a quiet
 * hold with the pump, before the pump stops. A quiet hold may not take the
 * pump from a diagnostic even before the diagnostic's first tick. */
static void flow_yield_now(loop_t *L)
{
    printf("K3. a quiet hold and a diagnostic's take each take the heater from the watch at once\n");
    close_session(L);
    run_idle_tick(L);
    put_long("thermal/water_pump_on", 1);
    int y = flow_yields;
    cool_quiet_hold_ex(1, 1);
    CHECK(flow_yields == y + 1 && flow_yield_quiet && flow_yield_pump == 1 &&
          get_long("thermal/water_pump_on") == 0,
          "a quiet hold with the pump: one yield, the hold already published, the pump not yet stopped");
    cool_quiet_hold(0);
    run_idle_tick(L);
    y = flow_yields;
    CHECK(cool_diag_take() == 0 && flow_yields == y + 1 && flow_yield_diag,
          "a diagnostic's take: one yield, the take already published");
    CHECK(!cool_quiet_pump_ok(), "a quiet hold may not take the pump from it, before its first tick");
    cool_diag_release();
    run_idle_tick(L);
}

static void gate_fans(loop_t *L)
{
    printf("gates D. a fan floor no fan meets: AIRFLOW after the grace and three ticks, for the session\n");
    close_session(L);
    put_long("thermal/tach_exhaust", EXHAUST_TACH_RUN);
    kv[0] = "cool_tach_exhaust_min_rpm"; kv[1] = "20000"; kv[2] = NULL;
    settle_and_open(L);
    double t0 = fake_now;               /* the session's open starts the grace */
    while (fake_now < t0 + FAN_GRACE_S + 1)
        run_tick(L);
    CHECK(!airflow_alarm && !strcmp(pub_verdict, "OK"), "the grace and two ticks under the floor: no fault yet");
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "AIRFLOW") && !pub_fire_ok && pub_hold && verdict_has("\"resume_ok\":false"),
          "the third tick past the grace: AIRFLOW, fire blocked, hold, no resume");
    CHECK(strstr(pub_reason, "exhaust") && fan_state_is("exhaust", "TRIPPED"),
          "the reason names the exhaust, its gate TRIPPED");
    CHECK(attn_latched && attn_sticky, "AIRFLOW is the fail tier: it takes the acknowledgment, its window still open");
    session_end_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && !pub_hold, "the session over: no hold, the fault for the operator");
    (void)cool_attention_ack();
    close_session(L);
    put_long("thermal/tach_exhaust", 300);
    kv[0] = "cool_purge_min_current"; kv[1] = "1023"; kv[2] = NULL;
    settle_and_open(L);
    for (int i = 0; i < (int)FAN_GRACE_S + 3; i++)
        run_tick(L);
    CHECK(fan_state_is("purge", "TRIPPED"), "a purge current floor of 1023: purge TRIPPED");
    close_session(L);
    kv[0] = "cool_tach_exhaust_min_rpm"; kv[1] = "0"; kv[2] = NULL;
    settle_and_open(L);
    CHECK(strstr(pub_gates_off, "\"exhaust\"") && fan_state_is("exhaust", "off"),
          "an exhaust floor of 0: exhaust in gates_off, its state off");
    kv[0] = NULL;
    close_session(L);
}

static void crash_watch(loop_t *L)
{
    printf("crash watch. never armed in an unarmed session; its three thresholds at 0 are its gates off\n");
    close_session(L);
    int arms = crash_arm_calls;
    for (int i = 0; i < 20; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    L->session = 1;
    for (int i = 0; i < 10; i++) {
        loop_tick(L);
        tick("run", 0);
    }
    CHECK(cool_state == Cool_Run && crash_arm_calls == arms && !strcmp(pub_accel_watch, "watch"),
          "an unarmed run session: crash_hw_arm never called, accel_watch watch");
    close_session(L);
    kv[0] = "cool_accel_x_alert"; kv[1] = "0"; kv[2] = "cool_accel_y_alert"; kv[3] = "0";
    kv[4] = "cool_accel_abort"; kv[5] = "0"; kv[6] = NULL;
    settle_and_open(L);
    CHECK(!strcmp(pub_verdict, "OK") && strstr(pub_gates_off, "\"crash_x_alert\"") &&
          strstr(pub_gates_off, "\"crash_y_alert\"") && strstr(pub_gates_off, "\"crash_abort\""),
          "the three thresholds at 0: OK, the three crash gates in gates_off");
    kv[0] = NULL;
    close_session(L);
}

/* The crash watch disarmed with BUMP standing. Nothing polls the head from
 * there, so the alert goes with the watch, and what it held waits for the
 * acknowledgment: nothing saw the head sit quiet. Left standing it held for
 * the rest of the session with the Acknowledge refused. */
static void crash_watch_stands_down(loop_t *L)
{
    printf("crash watch B. disarmed with BUMP standing: the alert ends with the watch and waits for the Acknowledge\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    crash_src1 = crash_src2 = 0;
    crash_poll_fail = 0;
    open_session(L);
    crash_src1 = 0x40 | 0x08;                       /* IG1: the alert generator, Y */
    run_tick(L);
    crash_src1 = 0;
    CHECK(!strcmp(pub_verdict, "BUMP") && crash_w.alert && attn_latched && !attn_sticky,
          "BUMP stands in the armed window");
    /* The window closes over it: the job still held, its report still run. */
    loop_tick(L);
    tick("run", 0);
    CHECK(!crash_armed_fake && !crash_w.alert, "the window closed: the watch is disarmed, and its alert went with it");
    CHECK(!strcmp(pub_verdict, "ACK") && !pub_fire_ok && attn_sticky && attn_notice_n == 0,
          "what it held waits for the acknowledgment: ACK, fire blocked, no notice of a clearing");
    for (int i = 0; i < 3; i++) {
        loop_tick(L);
        tick("run", 0);
    }
    CHECK(!strcmp(pub_verdict, "ACK"), "and it stays ACK, not BUMP for the rest of the session");
    CHECK(cool_attention_ack() == 0, "the Acknowledge is taken");
    loop_tick(L);
    tick("run", 0);
    CHECK(!strcmp(pub_verdict, "OK"), "and the session is free");

    /* The head stops answering with BUMP standing and the window open. */
    close_session(L);
    open_session(L);
    crash_src1 = 0x40 | 0x08;
    run_tick(L);
    crash_src1 = 0;
    crash_poll_fail = 1;
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "BUMP") && crash_w.alert, "two polls failed: BUMP still stands");
    run_tick(L);
    CHECK(!crash_w.alert && !strcmp(pub_accel_watch, "off"),
          "the third: the watch is down for the session, and its alert went with it");
    CHECK(!strcmp(pub_verdict, "ACK") && !pub_fire_ok && pub_hold && attn_notice_n == 0,
          "nothing saw the head sit quiet: ACK, fire blocked, the window held, no notice of a clearing");
    CHECK(cool_attention_ack() == 0, "the Acknowledge is taken");
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "and the job may go on");
    crash_poll_fail = 0;

    /* The fail tier's alarm is the session's, not the watch's. */
    close_session(L);
    open_session(L);
    crash_src2 = 0x40 | 0x02;                       /* IG2: the abort generator, X */
    run_tick(L);
    crash_src2 = 0;
    loop_tick(L);
    tick("run", 0);
    CHECK(!crash_armed_fake && crash_w.alarm && !strcmp(pub_verdict, "CRASH"),
          "a CRASH stands on past the disarm, to its session's end");
    close_session(L);
}

/* A session that opens over a FLAME standing from the last session's tail:
 * the hold stands on into it, and is released as any other. */
static void flame_over_session_open(loop_t *L)
{
    printf("O4. a session opening over a tail FLAME: the hold stands on, and nothing is said to have cleared\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    settle_and_open(L);
    run_tick(L);
    session_end_tick(L);                /* the window closed: the smoke phase */
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 400);               /* over the alert, under the critical */
    }
    for (int i = 0; i < 2; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    CHECK(cool_state == Cool_Smoke && !strcmp(pub_verdict, "FLAME") && attn_latched && !attn_sticky,
          "a flame in the smoke phase: FLAME, the tail's own");
    L->session = 1;
    run_tick(L);                        /* the next session opens over it */
    CHECK(cool_state == Cool_Run && !strcmp(pub_verdict, "FLAME") && pub_hold && !pub_fire_ok,
          "the session opens under it: still FLAME, hold, fire blocked");
    CHECK(attn_notice_n == 0, "and no notice says it cleared on its own");
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    for (int i = 0; i < FIRE_CLEAR_TICKS; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && attn_notice_n == 1,
          "five clean readings release it, and the notice is filed then");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
}

/* A limit that is not a finite number never stands. Every comparison against
 * a NaN is false, so such a limit would never trip and never read as off. */
static void nonfinite_tunables(loop_t *L)
{
    printf("gates E. a setting or a GFCOOL_ variable that is not a finite number: the default stands\n");
    close_session(L);
    kv[0] = "cool_temp_max"; kv[1] = "nan"; kv[2] = "cool_temp_critical_c"; kv[3] = "inf"; kv[4] = NULL;
    settle_and_open(L);
    CHECK(temp_max_c == TEMP_MAX_C_DEFAULT && temp_critical_c == TEMP_CRITICAL_C_DEFAULT,
          "nan and inf in the settings: both defaults stand");
    CHECK(!strcmp(pub_verdict, "OK"), "the session runs");
    L->bulk_c = 12.0;                   /* upstream past the default 33 C ceiling */
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && !pub_fire_ok && pub_hold,
          "and the ceiling still trips when the loop passes it");
    L->bulk_c = 0.0;
    kv[0] = NULL;
    close_session(L);
    setenv("GFCOOL_TEMP_MAX", "nan", 1);
    cool_init();
    unsetenv("GFCOOL_TEMP_MAX");
    CHECK(temp_max_c == TEMP_MAX_C_DEFAULT, "GFCOOL_TEMP_MAX=nan at the engine's start: the default stands");
    kv[0] = "cool_temp_max"; kv[1] = "35"; kv[2] = NULL;
    conf_reload();
    CHECK(temp_max_c == 35.0f, "and the variable is passed over, not pinned: the setting applies");
    kv[0] = NULL;
    conf_reload();
}

static void tec(loop_t *L)
{
    printf("TEC. fitted: on with the run airflow, off at idle, a dwell before it comes on again; not fitted: never\n");
    close_session(L);
    kv[0] = "cool_tec_present"; kv[1] = "1"; kv[2] = "cool_tec_on_c"; kv[3] = "15";
    kv[4] = "cool_tec_off_c"; kv[5] = "13"; kv[6] = NULL;
    settle_and_open(L);
    CHECK(get_long("thermal/tec_on") == 1, "fitted, its thresholds under the loop: on in a run session");
    close_session(L);
    double off_at = fake_now;           /* the tick that reached idle turned it off */
    CHECK(get_long("thermal/tec_on") == 0, "off at idle: no airflow");
    settle_and_open(L);
    CHECK(get_long("thermal/tec_on") == 0, "a run session inside the dwell: still off");
    while (get_long("thermal/tec_on") != 1 && fake_now - off_at < 120)
        run_tick(L);
    CHECK(get_long("thermal/tec_on") == 1 && fake_now - off_at >= TEC_DWELL_S,
          "on again once the dwell has passed, not before");
    close_session(L);
    kv[1] = "0";
    settle_and_open(L);
    int driven = get_long("thermal/tec_on") == 1;
    for (int i = 0; i < TEC_DWELL_S + 10; i++) {
        run_tick(L);
        if (get_long("thermal/tec_on") == 1)
            driven = 1;
    }
    CHECK(!driven, "not fitted: never written 1");
    kv[0] = NULL;
    close_session(L);
}

/* --- the attention's rules, the heater's, the recorder's ---------------- */

static void lid_ir_all(long v)
{
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, v);
    }
}

/* The holds the machine waits out on its own light nothing and leave
 * nothing to acknowledge: the warm-up, a cold loop and an extension's hold,
 * each the only cause in its session (a hot loop is the attention case's). */
static void self_clearing_holds(loop_t *L)
{
    static const struct {
        const char *verdict;
        const char *kv[3];
    } rows[] = {
        { "WARMUP", { "cool_temp_start", "30", NULL } },
        { "COLD",   { "cool_temp_min", "30", NULL } },
        { "EXT",    { "ext_enabled", "1", NULL } },
    };
    char msg[96];
    printf("the holds that clear on their own: WARMUP, COLD and EXT light nothing\n");
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        close_session(L);
        for (int i = 0; i < 3; i++)
            kv[i] = rows[r].kv[i];
        settle_and_open(L);
        if (!strcmp(rows[r].verdict, "EXT"))
            ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
        run_tick(L);
        snprintf(msg, sizeof(msg), "%s: no red, nothing for the operator", rows[r].verdict);
        CHECK(!strcmp(pub_verdict, rows[r].verdict) && !led_red && !attn_latched, msg);
        kv[0] = NULL;
    }
    unlink("cool-engine-test/holds/org.example.badge.json");
    close_session(L);
}

/* At most five notices stand: two pause-tier causes, each cleared inside
 * the window, in turn six times (a repeat of the last would count on it). */
static void notices_cap(loop_t *L)
{
    printf("the notices: at most five stand, a sixth drops the oldest\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    crash_src1 = crash_src2 = 0;
    crash_poll_fail = 0;
    unsigned base = attn_notice_seq;
    settle_and_open(L);
    run_tick(L);
    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0) {
            L->dead_down = 1;                       /* SENSOR */
            for (int k = 0; k < 3; k++)
                run_tick(L);
            L->dead_down = 0;
            run_tick(L);
        } else {
            crash_src1 = 0x40 | 0x08;               /* BUMP: IG1, the alert generator, Y */
            run_tick(L);
            crash_src1 = 0;
            for (int k = 0; k < 6; k++)
                run_tick(L);
        }
    }
    CHECK(attn_notice_n == COOL_NOTICES_N && attn_notice_seq == base + 6 &&
          attn_notices[0].seq == base + 2 && attn_notices[COOL_NOTICES_N - 1].seq == base + 6,
          "six causes cleared in the window: five notices stand, the oldest gone");
    cool_notices_dismiss((unsigned)-1);
    close_session(L);
}

/* The notices document: a reason's quote and backslash are made plain
 * before it reaches /cool/status, and a list past the budget is published
 * as none, never cut. The engine's own reasons hold neither, so the cause
 * is raised on attention_tick itself. */
static void notices_json_safe(loop_t *L)
{
    char doc[COOL_STATUS_JSON_MAX];
    printf("the notices document: JSON-safe, never cut\n");
    close_session(L);
    run_idle_tick(L);
    cool_notices_dismiss((unsigned)-1);
    snprintf(pub_reason, sizeof(pub_reason), "a \"quoted\" word and a \\ backslash");
    attention_tick("SENSOR");
    attention_tick("OK");               /* cleared at idle, nothing ended: a notice */
    pub_reason[0] = '\0';               /* the verdict's own words gone with it */
    cool_status_json(doc, sizeof(doc));
    json_t *j = json_loads(doc, 0, NULL);
    const char *why = json_string_value(
        json_object_get(json_array_get(json_object_get(j, "notices"), 0), "reason"));
    CHECK(why && !strpbrk(why, "\"\\"), "a quote and a backslash in a reason: the document parses, the reason plain");
    if (j)
        json_decref(j);
    for (int i = 0; i < COOL_NOTICES_N; i++) {
        attn_notice_t *a = &attn_notices[i];
        a->seq = 4294967295u;
        snprintf(a->verdict, sizeof(a->verdict), "CRITICAL");
        memset(a->reason, 'x', sizeof(a->reason) - 1);
        a->reason[sizeof(a->reason) - 1] = '\0';
        a->at = a->cleared_at = 1e15;
        a->times = 2147483647;
    }
    attn_notice_n = COOL_NOTICES_N;
    notices_publish();
    CHECK(!strcmp(pub_notices, "[]"), "five notices past the budget: published as none, never cut");
    attn_notice_n = 0;
    notices_publish();
    run_idle_tick(L);
}

/* A new cause and the record the panel asks about: a record that waits for
 * its acknowledgment stays (the new cause shows as the verdict); a record
 * that is not sticky gives way to the next cause. */
static void attention_takeover(loop_t *L)
{
    printf("the attention's record: a waiting acknowledgment keeps it, one not sticky gives way\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    attn_keep = 1;
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_temp_critical_c"; kv[5] = "20"; kv[6] = NULL;
    settle_and_open(L);
    run_tick(L);
    kv[0] = NULL;
    close_session(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && !strcmp(attn_verdict, "CRITICAL"), "CRITICAL waits for its acknowledgment");
    settle_and_open(L);
    L->dead_down = 1;
    for (int k = 0; k < 3; k++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && !strcmp(attn_verdict, "CRITICAL"),
          "a new cause under it: the verdict SENSOR, the record still CRITICAL's");
    L->dead_down = 0;
    run_tick(L);
    CHECK(cool_attention_ack() == 0, "the Acknowledge takes it");
    close_session(L);
    lid_ir_all(100);
    settle_and_open(L);
    L->dead_down = 1;
    for (int k = 0; k < 3; k++)
        run_tick(L);
    CHECK(!strcmp(attn_verdict, "SENSOR") && attn_latched && !attn_sticky, "SENSOR stands in the window, not sticky");
    lid_ir_all(300);                    /* q1 past its alert, under its critical */
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FLAME") && !strcmp(attn_verdict, "FLAME"), "FLAME over it: the record is FLAME's");
    lid_ir_all(100);
    L->dead_down = 0;
    for (int k = 0; k < FIRE_CLEAR_TICKS + 1; k++)
        run_tick(L);
    attn_keep = 0;
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    for (int i = 1; i <= 4; i++) {
        char a[64];
        snprintf(a, sizeof(a), GF_SYSFS "pic/lid_ir_%d", i);
        unlink(a);
    }
}

/* With the flow check gone, nothing but a warm-up heats the loop: an armed
 * session past the old check's settle and run, and its tail to idle. */
static void no_engine_heat(loop_t *L)
{
    printf("no flow check: the engine heats nothing outside a warm-up\n");
    close_session(L);
    put_long("thermal/heater_pwm", 0);
    int heated = 0;
    settle_and_open(L);
    for (int i = 0; i < 120; i++) {
        run_tick(L);
        heated |= get_long("thermal/heater_pwm") != 0;
    }
    L->session = 0;
    for (int i = 0; i < 900 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        tick("idle", 0);
        heated |= get_long("thermal/heater_pwm") != 0;
    }
    CHECK(!heated && heater_pct == 0 && cool_state == Cool_Idle,
          "120 s armed at a normal coolant, and the tail to idle: the heater never above 0");
}

/* The engine's warnings that change no verdict ask the flight recorder for
 * the minutes around them, each once (the silent controller is M's). */
static void recorder_warnings(loop_t *L)
{
    printf("the warnings that change no verdict arm the flight recorder\n");
    close_session(L);
    for (int i = 0; i < 4; i++)
        run_idle_tick(L);               /* the armed window well past */
    int ev = events;
    put_long("cnc/laser_on_sampled", 5);
    run_idle_tick(L);
    put_long("cnc/laser_on_sampled", 0);
    run_idle_tick(L);
    CHECK(events == ev + 1, "an emission with no armed window");
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    long pgood = get_long("cnc/laser_pgood_sampled");
    put_long("cnc/laser_pgood_sampled", 255);
    settle_and_open(L);
    ev = events;
    put_long("cnc/laser_pgood_sampled", 50);
    run_tick(L);
    run_tick(L);
    CHECK(events == ev + 1, "a power-good drop in the armed window");
    put_long("cnc/laser_pgood_sampled", pgood);
    ev = events;
    put_long("cnc/faults", 4);
    run_tick(L);
    run_tick(L);
    put_long("cnc/faults", 0);
    run_tick(L);
    CHECK(events == ev + 1, "a stepper driver fault in the session");
    ev = events;
    throttle_stub = 1;
    run_tick(L);
    run_tick(L);
    throttle_stub = 0;
    run_tick(L);
    CHECK(events == ev + 1, "a CPU throttle");
    close_session(L);
    kv[0] = "cool_cooldown_max_s"; kv[1] = "5"; kv[2] = NULL;
    settle_and_open(L);
    L->bulk_c = 9.7;                    /* over the resume line once the run airflow's offset is gone */
    run_tick(L);
    ev = events;
    session_end_tick(L);
    for (int i = 0; i < 60 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    CHECK(cool_state == Cool_Idle && events == ev + 1, "a thermal cooldown that times out above the resume line");
    L->bulk_c = 0.0;
    kv[0] = NULL;
    close_session(L);
}

/* The flow check's settings are gone with it: no tunable reads them and no
 * bench override names them. */
static void retired_tunables(void)
{
    static const char *const keys[] = {
        "cool_flow_rise", "cool_flow_heater_pct", "cool_flow_check_s", "cool_recheck_s",
        "cool_confirm_max_s", "cool_laser_heat_density", "cool_laser_heat_cw" };
    static const char *const envs[] = {
        "GFCOOL_FLOW_RISE", "GFCOOL_FLOW_HEATER_PCT", "GFCOOL_FLOW_CHECK_S", "GFCOOL_RECHECK_S",
        "GFCOOL_CONFIRM_MAX_S", "GFCOOL_LASER_HEAT_DENSITY", "GFCOOL_LASER_HEAT_CW" };
    printf("the retired flow-check settings\n");
    int found = 0;
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++)
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            found += !strcmp(tunables[i].key, keys[k]) || !strcmp(tunables[i].env, envs[k]);
    CHECK(!found, "no tunable reads a retired key, and no GFCOOL_ variable names one");
}

/* The shutdown stops the flow watch, its heater off, before the engine
 * writes its own heater off: nothing of the watch's runs after the last
 * write. The engine's heat stands at the watch's stop (a warm-up), so the
 * order shows. Last: the engine is not started again. */
static void shutdown_order(loop_t *L)
{
    printf("the shutdown: the flow watch stopped first, then the engine's heater off\n");
    close_session(L);
    kv[0] = "cool_temp_start"; kv[1] = "30"; kv[2] = NULL;
    settle_and_open(L);
    long heat = get_long("thermal/heater_pwm");
    kv[0] = NULL;
    flow_stops = 0;
    engine_run = 0;                     /* no thread to join: the test ticks the engine itself */
    cool_shutdown();
    CHECK(heat > 0 && flow_stops == 1 && flow_stop_heater == heat && get_long("thermal/heater_pwm") == 0,
          "the watch stopped once with the warm-up's heat still on, then the heater off");
}

int main(void)
{
    make_tree();
    /* The fan writes and the intakes fitted run on a loop at rest; the
     * tree is laid again after them. */
    fan_writes();
    intakes();
    make_tree();
    kv[0] = NULL;
    loop_t L = { 0 };

    printf("G. the air-assist offset is taken off in counts while the fan runs\n");
    /* The fan's ground lift raises the raw counts, and more counts read
       colder. The model writes raw counts with no such lift in them, so
       with the setting at 20 the engine must read both sensors 20 counts
       LOWER than the raw (warmer) while the run profile is on, and the raw
       when it is not. */
    kv[0] = "cool_aa_offset_counts"; kv[1] = "20"; kv[2] = NULL;
    open_session(&L);
    CHECK(cool_coolant_offset_counts() == 20, "the run profile commands the fan: 20 counts added");
    {
        long raw = get_long("pic/water_temp_2");            /* the model's last raw upstream */
        float engine_up = pub_up;
        float expect = (float)coolant_degc(raw - 20);
        CHECK(fabs(engine_up - expect) < 0.05,
              "the published upstream reading is the raw less 20 counts");
        CHECK(engine_up > (float)coolant_degc(raw) + 0.8,
              "the correction is worth about a degree at this temperature");
    }
    close_session(&L);
    CHECK(cool_coolant_offset_counts() == 0, "the fan idle: no counts added");
    kv[0] = "cool_aa_offset_counts"; kv[1] = "500"; kv[2] = NULL;
    conf_reload();
    aa_write(1023);
    CHECK(cool_coolant_offset_counts() == 60, "an absurd setting is bounded at 60 counts");
    aa_write(600);
    CHECK(cool_coolant_offset_counts() > 20 && cool_coolant_offset_counts() < 40,
          "a part duty adds part of the setting (600 of 1023)");
    aa_write(204);
    kv[0] = NULL;
    conf_reload();
    /* The flow watch reads its 45 C rail through the engine's two
     * corrections, so the context carries both; and a sample's degrees
     * (the sensors logger's) are the engine's own reading of the raw. */
    kv[0] = "cool_aa_offset_counts"; kv[1] = "20"; kv[2] = "cool_temp_offset_c"; kv[3] = "0.5"; kv[4] = NULL;
    open_session(&L);
    CHECK(fabs(flow_ctx.offset_counts - 20.0) < 1e-9 && fabs(flow_ctx.temp_offset_c - 0.5) < 1e-6,
          "the run profile: the watch's context carries the 20-count shift and the 0.5 C correction");
    CHECK(fabs(cool_coolant_c(get_long("pic/water_temp_2")) - pub_up) < 0.01,
          "a sample's degrees from the raw are the engine's own reading, both corrections in");
    close_session(&L);
    CHECK(flow_ctx.offset_counts == 0.0 && fabs(flow_ctx.temp_offset_c - 0.5) < 1e-6,
          "idle: no shift in the context, the correction still in it");
    kv[0] = NULL;
    conf_reload();

    printf("H. a coolant sensor that stops reading mid-run\n");
    open_session(&L);
    run_tick(&L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "a settled session: fire permitted");
    L.dead_up = 1;
    run_tick(&L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "one failed read is tolerated");
    run_tick(&L);
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR"),
          "two failed reads: SENSOR, fire blocked, hold");
    CHECK(strstr(last_log, "upstream") != NULL, "the line names the sensor");
    CHECK(heater_pct == 0, "the heater is off");
    for (int i = 0; i < 5; i++)
        run_tick(&L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR"),
          "and it stays so while the sensor is dead");
    L.dead_up = 0;
    run_tick(&L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "the reading back: released at once");
    L.dead_down = 1;
    run_tick(&L);
    run_tick(&L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR") && strstr(last_log, "downstream") != NULL,
          "the downstream sensor alone is enough to blind the engine");
    L.dead_down = 0;

    printf("J. blind under the floor: COLD keeps its state\n");
    kv[0] = "cool_temp_min"; kv[1] = "30"; kv[2] = NULL;   /* the loop at 22.3 C is under it */
    open_session(&L);
    run_tick(&L);
    CHECK(cold_gate && !pub_fire_ok && !strcmp(pub_verdict, "COLD"),
          "the loop under the floor: COLD, fire blocked");
    L.dead_up = 1;
    for (int i = 0; i < 3; i++)
        run_tick(&L);
    CHECK(cold_gate, "blind: the floor keeps its state");
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR"), "and the verdict says SENSOR");
    L.dead_up = 0;
    run_tick(&L);
    CHECK(cold_gate && !pub_fire_ok && !strcmp(pub_verdict, "COLD"),
          "reading again: still COLD, never a moment of fire");
    kv[0] = NULL;
    open_session(&L);           /* the floor back at its default */
    run_tick(&L);
    CHECK(!cold_gate && !strcmp(pub_verdict, "OK"), "the floor back at its default: the gate clears");
    close_session(&L);

    printf("K. blind in a warm-up: the heater goes off and comes back\n");
    kv[0] = "cool_temp_start"; kv[1] = "30"; kv[2] = NULL;
    flow_yields = 0;
    open_session(&L);
    CHECK(cool_state == Cool_Warmup && heater_pct > 0 && !strcmp(pub_verdict, "WARMUP"),
          "the session opens under the start gate: warm-up, heater on");
    CHECK(flow_yields == 1 && flow_yield_heater == 0,
          "the flow watch gave the heater back before the warm-up wrote it");
    run_tick(&L);
    CHECK(flow_ctx.yield && !strcmp(flow_ctx.yield_why, "warm-up") && !flow_ctx.session,
          "and every warm-up tick keeps it away: no session for the watch yet");
    L.dead_up = 1;
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && heater_pct == 0,
          "blind: SENSOR, the heater off");
    CHECK(flow_yields == 2, "the blind tick took the heater back from the flow watch too");
    L.dead_up = 0;
    run_tick(&L);
    CHECK(cool_state == Cool_Warmup && !strcmp(pub_verdict, "WARMUP") && heater_pct > 0,
          "reading again: the warm-up holds on with its heater back");
    /* The kernel's lease: a heater nobody writes again goes off. Every
     * warm-up tick writes it again. */
    put_long("thermal/heater_pwm", 0);
    run_tick(&L);
    CHECK(get_long("thermal/heater_pwm") == (long)WARMUP_HEATER_PCT * 65535 / 100,
          "every warm-up tick writes the heater again (the kernel lease)");
    /* Downstream at the cutoff: the heater off, the hold stays; cool again
     * under the resume line: back on. Every gate reads upstream. Both lines
     * are held to half a degree either side: off at 45 C, on again at 40 C.
     * The downstream reading is written as it is, with no loop model over
     * it. */
    put_long("pic/water_temp_1", raw_for(44.5));
    tick("run", 1);
    CHECK(heater_pct > 0 && cool_state == Cool_Warmup, "downstream at 44.5 C: under the cutoff, the heater stays on");
    put_long("pic/water_temp_1", raw_for(45.5));
    tick("run", 1);
    CHECK(get_long("thermal/heater_pwm") == 0 && heater_pct == 0 && cool_state == Cool_Warmup &&
          !strcmp(pub_verdict, "WARMUP") && log_has("cutoff - heater off"),
          "downstream at 45.5 C: past the 45 C cutoff, the warm-up heater off, the hold stays");
    put_long("pic/water_temp_1", raw_for(40.5));
    tick("run", 1);
    CHECK(heater_pct == 0 && cool_state == Cool_Warmup, "downstream at 40.5 C: not yet under the 40 C line, the heater stays off");
    put_long("pic/water_temp_1", raw_for(39.5));
    tick("run", 1);
    CHECK(heater_pct > 0 && cool_state == Cool_Warmup, "downstream at 39.5 C: under it, the heater on again");
    /* The pump held off by a quiet hold: no heat. */
    CHECK(!cool_quiet_pump_ok(), "a quiet hold may not take the pump under a warm-up");
    quiet_pump_held = 1;
    run_tick(&L);
    CHECK(heater_pct == 0 && get_long("thermal/heater_pwm") == 0, "the pump held off: the warm-up heater off");
    quiet_pump_held = 0;
    run_tick(&L);
    CHECK(heater_pct > 0, "the pump back: the heater on again");
    /* The budget: 30 minutes of heat without reaching the gate. */
    fake_now += (double)WARMUP_HEAT_MAX_S;
    run_tick(&L);
    CHECK(heater_pct == 0 && get_long("thermal/heater_pwm") == 0 && cool_state == Cool_Warmup &&
          !strcmp(pub_verdict, "WARMUP") && strstr(pub_reason, "heater off after 30 min"),
          "30 min of warm-up heat: the heater off for good, the hold stays and says why");
    run_tick(&L);
    CHECK(heater_pct == 0, "and it stays off");
    kv[0] = NULL;
    close_session(&L);
    CHECK(cool_quiet_pump_ok(), "idle again: a quiet hold may take the pump");

    flow_yields_table(&L);
    flow_yield_now(&L);

    test_ack_waits_for_fans(&L);

    /* The safing attributes the engine writes: present in the tree as
       "0" so a write can be seen, and put back to "0" between ticks so
       a repeat can be told from a single write. */
    printf("M. the hang dead-man: once while armed, every tick while the kernel runs\n");
    open_session(&L);
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    logbuf[0] = '\0';
    /* The tolerance side first: a report 4.5 s old is still fresh.
     * Nothing is safed, and the armed window stays acknowledged. The
     * case drives report silence: a silent controller, or the whole
     * daemon stalled. A stall of the engine's own thread is not silence,
     * because the route records each report. The cooling engine page's
     * Silence bullet gives the rule. */
    fake_now += 4.5;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch") == 0 && pub_armed_ack &&
          !log_has("controller silent"),
          "4.5 s without a report: nothing safed, the window still acknowledged");
    fake_now += 1.5;                    /* 6.0 s: past REPORT_TIMEOUT_S with no report */
    int ev0 = events;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch") == 1,
          "silence while armed: motion stopped and the latch locked");
    CHECK(events == ev0 + 1, "and the flight recorder is asked for the minutes around it, once");
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    fake_now += 1.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch") == 0,
          "the armed case writes once per silence");
    put("cnc/state", "running\n");
    for (int i = 0; i < 3; i++) {
        put("cnc/stop", "0\n");
        put("cnc/laser_latch", "0\n");
        fake_now += 1.0;
        loop_tick(&L);
        engine_tick();
        CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch") == 1,
              "the kernel still running: the pair is written again this tick");
    }
    put("cnc/state", "idle\n");
    close_session(&L);

    diag_heater_lease(&L);

    printf("N. the fail tiers end the controller through the supervisor, once\n");
    cool_fail_tier_stop = fake_stop;
    stops = 0;
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 100);
    }
    open_session(&L);
    CHECK(!strcmp(pub_verdict, "OK") && stops == 0, "a lit lid at its baseline: OK, no stop");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 800);                        /* q1 past its critical, q2 under its own */
    }
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "FIRE") && !strcmp(pub_fire_watch, "ALARM"),
          "the q1 quartile past its critical threshold: FIRE, the watch ALARM");
    CHECK(stops == 1 && !strcmp(stop_why, "lid IR fire signal"),
          "FIRE ends the controller through the hook, once, naming the signal");
    CHECK(stop_seen_stop == 1 && stop_seen_latch == 1,
          "the kernel writes had landed when the hook was called");
    run_tick(&L);
    run_tick(&L);
    CHECK(stops == 1 && !strcmp(pub_verdict, "FIRE"), "the tier stands, the hook is not called again");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 100);
    }
    close_session(&L);
    stops = 0;
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    crash_src1 = crash_src2 = 0;
    open_session(&L);
    CHECK(crash_armed_fake && stops == 0, "the crash watch is armed with the session, no stop");
    crash_src2 = 0x40 | 0x02;                       /* IG2: the abort generator, X */
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "CRASH"), "the abort generator: CRASH");
    CHECK(stops == 1 && !strcmp(stop_why, "head crash signal"),
          "CRASH ends the controller through the hook, once, naming the signal");
    CHECK(stop_seen_stop == 1 && stop_seen_latch == 1,
          "the kernel writes had landed when the hook was called");
    run_tick(&L);
    CHECK(stops == 1, "the tier stands, the hook is not called again");
    CHECK(attn_latched && attn_sticky && !strcmp(attn_verdict, "CRASH"),
          "CRASH is the fail tier: it takes the acknowledgment to end it");
    crash_src2 = 0;
    close_session(&L);

    printf("O. the pause tiers hold the job and call nothing\n");
    stops = 0;
    open_session(&L);
    crash_src1 = 0x40 | 0x08;                       /* IG1: the alert generator, Y */
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "BUMP") && stops == 0, "the alert generator: BUMP, no stop");
    CHECK(attn_latched && !attn_sticky, "BUMP needs the operator, and is not the fail tier");
    crash_src1 = 0;
    unsigned seq_bump = attn_notice_seq;
    for (int i = 0; i < 6; i++)
        run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && stops == 0, "quiet polls release the bump, no stop");
    CHECK(!attn_latched && attn_notice_seq == seq_bump + 1,
          "a bump that clears inside the window leaves a notice, no acknowledgment");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 300);                        /* q1 past its alert, q2 under its own */
    }
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "FLAME") && pub_hold && !strcmp(pub_fire_watch, "alert") && stops == 0,
          "the q1 quartile over its alert threshold: FLAME, hold, the watch alert, no stop");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 100);
    }
    open_session(&L);
    CHECK(!strcmp(pub_verdict, "OK"), "a new session after the restore: OK");

    flame_blind_tick(&L);

    printf("O2. the fire watch's four thresholds at 0 are its gates off\n");
    close_session(&L);
    kv[0] = "cool_fire_q1_alert"; kv[1] = "0"; kv[2] = "cool_fire_q1_critical"; kv[3] = "0";
    kv[4] = "cool_fire_q2_alert"; kv[5] = "0"; kv[6] = "cool_fire_q2_critical"; kv[7] = "0";
    kv[8] = NULL;
    settle_and_open(&L);
    CHECK(strstr(pub_gates_off, "\"flame_q1_alert\"") && strstr(pub_gates_off, "\"flame_q1_critical\"") &&
          strstr(pub_gates_off, "\"flame_q2_alert\"") && strstr(pub_gates_off, "\"flame_q2_critical\""),
          "the four flame gates are in gates_off");
    CHECK(!strcmp(pub_fire_watch, "watch"), "the watch reads watch");
    close_session(&L);
    kv[0] = NULL;
    settle_and_open(&L);
    CHECK(!strcmp(pub_fire_watch, "armed"), "restored, the watch reads armed");
    close_session(&L);

    printf("P. an extension's hold joins the pause tier, under every verdict of the engine's own\n");
    mkdir("cool-engine-test/holds", 0755);
    holds_dir = "cool-engine-test/holds";
    mkdir("cool-engine-test/required-holds", 0755);
    holds_required_dir = "cool-engine-test/required-holds";
    ext_safe_file = "cool-engine-test/ext-safe";
    unlink(ext_safe_file);
    stops = 0;
    open_session(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && !pub_hold, "a settled session before any hold: OK");
    ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "extensions off: a raised hold is not looked at");
    kv[0] = "ext_enabled"; kv[1] = "1"; kv[2] = NULL;
    ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "EXT") && !pub_fire_ok && pub_hold && stops == 0 &&
          !strcmp(pub_reason, "org.example.badge: no badge presented"),
          "extensions on: EXT, fire withheld, the job held, nothing stopped, the reason the package's");
    /* the engine's own verdicts outrank it, and it never lifts one */
    crash_src1 = 0x40 | 0x08;
    ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "BUMP") && !pub_fire_ok && pub_hold, "a bump under a hold reads BUMP");
    crash_src1 = 0;
    for (int i = 0; i < 6; i++) {
        ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
        run_tick(&L);
    }
    CHECK(!strcmp(pub_verdict, "EXT") && !pub_fire_ok &&
          !strcmp(pub_reason, "org.example.badge: no badge presented"),
          "the bump released: EXT again, with the hold's own words back");
    /* cleared by the host */
    ext_hold_file("org.example.badge", 1, 0, "", fake_now + 1.0);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok && !pub_hold && !pub_reason[0],
          "cleared: OK, fire back, the words gone");
    /* the operator's two exits need no host */
    ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "EXT"), "raised again: EXT");
    { FILE *f = fopen(ext_safe_file, "w"); if (f) fclose(f); }
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "safe mode ends the hold at the next tick");
    unlink(ext_safe_file);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "EXT"), "out of safe mode it stands again");
    kv[0] = NULL;
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && pub_fire_ok, "extensions off ends the hold at the next tick");
    /* a fail tier is a fail tier under a hold too */
    kv[0] = "ext_enabled"; kv[1] = "1"; kv[2] = NULL;
    unlink("cool-engine-test/holds/org.example.badge.json");
    ext_hold_file("org.example.filter", 0, 1, "filter life under 5 percent", fake_now + 1.0);
    crash_src2 = 0x40 | 0x02;
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "CRASH") && stops == 1, "CRASH under a hold: CRASH, and the controller is ended");
    crash_src2 = 0;
    unlink("cool-engine-test/holds/org.example.filter.json");
    kv[0] = NULL;
    close_session(&L);
    cool_fail_tier_stop = NULL;

    printf("Q. the verdict file: the contract's keys and types, the stamp to the millisecond and never ahead of the clock\n");
    open_session(&L);
    /* A fraction the nearest millisecond rounds up, and any coarser
     * decimal grid rounds down by more than 9 ms. */
    fake_now = floor(fake_now) + 1.9996;
    run_tick(&L);
    json_t *doc = json_load_file(VERDICT_FILE, 0, NULL);
    CHECK(doc && json_is_object(doc), "the verdict file parses as an object");
    CHECK(doc && json_is_integer(json_object_get(doc, "seq")) && json_is_number(json_object_get(doc, "ts_mono")) &&
          json_is_boolean(json_object_get(doc, "fire_ok")) && json_is_string(json_object_get(doc, "verdict")) &&
          json_is_boolean(json_object_get(doc, "hold")) && json_is_boolean(json_object_get(doc, "resume_ok")) &&
          json_is_boolean(json_object_get(doc, "armed")) && json_is_boolean(json_object_get(doc, "ack_wait")) &&
          json_is_string(json_object_get(doc, "reason")) &&
          json_is_number(json_object_get(doc, "down_c")) && json_is_number(json_object_get(doc, "up_c")),
          "the documented keys with their documented types");
    double ts = doc ? json_number_value(json_object_get(doc, "ts_mono")) : 0.0;
    printf("    ts_mono %.4f at the clock's %.4f\n", ts, fake_now);
    CHECK(ts <= fake_now && fake_now - ts < 0.001,
          "ts_mono is the engine's monotonic clock to the millisecond, never ahead of it");
    CHECK(doc && json_is_true(json_object_get(doc, "resume_ok")) == !json_is_true(json_object_get(doc, "hold")),
          "resume_ok is the complement of hold");
    if (doc)
        json_decref(doc);
    close_session(&L);

    attention(&L);
    diag_keeps_attention(&L);
    phases(&L);
    gate_ceiling(&L);
    gate_warmup(&L);
    gate_critical(&L);
    gate_fans(&L);
    crash_watch(&L);
    crash_watch_stands_down(&L);
    flame_over_session_open(&L);
    nonfinite_tunables(&L);
    tec(&L);
    self_clearing_holds(&L);
    notices_cap(&L);
    notices_json_safe(&L);
    attention_takeover(&L);
    no_engine_heat(&L);
    recorder_warnings(&L);
    retired_tunables();
    shutdown_order(&L);

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the engine takes the air-assist offset off while the fan "
                      "runs and says SENSOR "
                      "rather than fire blind; every fan duty is read back and held, "
                      "and one intake fan fitted leaves the second tach unjudged; the "
                      "gates trip past the loop and are off at their far ends, the "
                      "phases end at their duties, and the TEC keeps its rules\n",
           failures);
    return failures ? 1 : 0;
}
