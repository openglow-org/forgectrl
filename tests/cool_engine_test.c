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
 *   H. blind mid-run:       one failed read tolerated and said, two are
 *                           SENSOR (fire blocked, hold, heater off),
 *                           released after four readings in a row
 *   H3. a dropping sensor:  every other tick is SENSOR, and stays so; every
 *                           third tick is tolerated
 *   H2. no temperature:     a count of 0, the rail and past it, or one
 *                           under 0 C is no reading, judged on the count
 *                           before the air-assist lift and the correction
 *   J. blind under the floor: COLD keeps its state through SENSOR
 *   K. blind in a warm-up:  the heater goes off and comes back
 *   L. the acknowledgment:  it waits for the fans; one low tach reading
 *                           keeps it and the airflow gate's count takes
 *                           it, in the spin-up grace too, and then the
 *                           window it was given holds (FANS) until the
 *                           fan reads its floor; a window open before the
 *                           engine started holds the same way until its
 *                           fans read their floors; CRITICAL, SENSOR and
 *                           AIRFLOW name themselves over FANS, with
 *                           their own words; a window that
 *                           opens is acknowledged on its own fans; a
 *                           quiet hold and a reserved button block fire
 *                           and hold an open window, the reservation's
 *                           package's own window excepted while it keeps
 *                           the machine; a renewal moves a reservation's
 *                           end, and each prompt's is its own; the window
 *                           is acknowledged only once every extension
 *                           package's group is still (frozen, at the job's
 *                           share of the core, or empty), never while
 *                           the groups do not read, and only on the
 *                           extension host's word for this window; the
 *                           acknowledgment is the window's own: off the
 *                           file at its close, never put back by a tick
 *                           that judged the window before, and a window
 *                           that closes and opens between ticks, or after
 *                           a stale report, is a new one
 *   M. the hang dead-man:   a report under the timeout is still fresh and
 *                           acknowledged; silence while armed stops and
 *                           locks once; silence while the kernel runs
 *                           repeats every tick, and a kernel state that
 *                           cannot be read counts as running for 5 s
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
 *                           grace, so a fan that then runs is not tripped,
 *                           while the acknowledged window holds (FANS)
 *                           until it reads its floor again
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
 *   phases C.  a busy start's end under OVERTEMP keeps the cooling airflow
 *   gates A.   the ceiling under the loop: OVERTEMP at the run start; at
 *              60 it is off, said so
 *   gates B.   the start gate over the loop: WARMUP with the heater on and
 *              the fans idle, released on the minute's minimum, not on a
 *              slug; both low gates at 0 are off
 *   gates B2.  no upstream reading at the session's start: WARMUP
 *              undecided, no heat, no acknowledgment, until one decides;
 *              a run so decided is acknowledged on its own fans
 *   gates E.   a value the setting does not take (not finite, padded, too
 *              long, out of range): the default stands
 *   gates F.   an upstream reading over 70 C releases no cold-side gate,
 *              and drives no TEC
 *   O5.        a lid IR channel unreadable: SENSOR at the second tick, the
 *              others judged meanwhile
 *   gates C.   the critical line under the loop: CRITICAL, no resume, gone
 *              with the session; at 70 it is off and the ceiling pauses
 *   gates D.   a fan floor no fan meets: AIRFLOW after the grace and three
 *              ticks, gone with the session; a floor of 0 is off
 *   crash watch: never armed in an unarmed session; its thresholds at 0
 *              are its gates off; a watch that cannot open or arm is off
 *              for its session alone
 *   TEC.       fitted: on with the run airflow, off at idle, a dwell before
 *              it comes on again; not fitted: never
 *   TEC2.      the pump and the TEC written every tick: a refused TEC
 *              write is written again, a pump found off is put back on
 *   TEC3.      a write that keeps failing is named once, until one takes
 *   gates A2.  OVERTEMP's cooling airflow is a state: it outlasts a thermal
 *              phase that times out and a quiet hold's release, keeps the
 *              TEC to its airflow, refuses a diagnostic and a pump hold
 *              (one set past the route's check is not applied), and goes
 *              with the recovery
 *   gates A3.  a diagnostic taken inside the tick that raises OVERTEMP:
 *              its release puts the cooling airflow on
 *   phases D.  a smoke clear that ends into idle under a standing gate
 *              lands on the cooling airflow
 *   quiet A/B. the quiet hold's posture is the engine's: taken again
 *              without the pump it gives the pump back, and a session's
 *              opening tick inside its call leaves none of it behind
 *   quiet C/D. a pump hold set after a session opened is not applied in
 *              it; one applied at idle is given back before a warm-up's
 *              heater
 *   quiet E-G. a pump hold set inside a session's opening tick (into its
 *              run, into its warm-up, released again inside the tick): the
 *              tick ends with the pump on and no heater on a stopped pump
 *   quiet H.   the tick that raises OVERTEMP gives an applied pump hold's
 *              pump back
 *   quiet J.   a pump hold standing in a session (set past the route) is
 *              applied in no warm-up, smoke clear or cooldown, and by
 *              itself at the idle after them
 *   quiet K/L. a hold the route asks for in a session's opening tick after
 *              its edge is refused: the run keeps its fans, the warm-up its
 *              heater
 *   quiet M.   a pump hold past its bound is released, the pump back on
 *   quiet N/O. a fans-only hold in an armed run is refused and a release
 *              taken; a pump hold in the smoke clear is refused as a busy
 *              engine
 *   quiet P.   a hold standing when the coolant crosses its ceiling is
 *              released in the rising tick: the cooling airflow and the
 *              TEC as with no hold, the reason OVERTEMP's own; and (Ps) a
 *              hold taken in a cooldown, the ceiling crossed in it
 *   quiet Q.   a request right after that release is refused, and a pump
 *              hold set past the route there is not applied under the gate
 *   diagnostics B. a tool's fan duties are taken back to the phase's at
 *              the diagnostic's release
 *
 * And after them: the holds that clear on their own light nothing; at most
 * five notices, their document JSON-safe and never cut; a waiting
 * acknowledgment keeps its record; the flow watch's context (its fans and
 * run session, the two corrections) and the yields that land at once (K3);
 * no engine heat outside a warm-up; the warnings that arm the flight
 * recorder; the retired flow-check settings; the shutdown's order, the TEC
 * off on both sides of the engine's join.
 */
#define GF_SYSFS    "cool-engine-test/sys/"
#define VERDICT_DIR "cool-engine-test/run"
#define VERDICT_FILE VERDICT_DIR "/cooling.state"
#define EXT_CG_DIR  "cool-engine-test/ffx"
#define EXT_HOST_STATUS "cool-engine-test/ext/status.json"
#define clock_gettime fake_clock_gettime
/* The head accelerometer's interrupt generators, faked: the engine's
   calls land here (the real ones open an i2c bus this host lacks). */
#define crash_hw_open   fake_crash_open
#define crash_hw_arm    fake_crash_arm
#define crash_hw_poll   fake_crash_poll
#define crash_hw_disarm fake_crash_disarm
#define crash_hw_close  fake_crash_close
/* The engine's start runs here without its thread: the cases tick it. The
 * shutdown's join is noted with the TEC as it found it. */
#define pthread_create  fake_pthread_create
#define pthread_join    fake_pthread_join
/* The fake tree is plain files, and the engine's writes truncate them
 * (O_TRUNC, which sysfs ignores), as a sysfs store replaces: without it a
 * short duty leaves the tail of a longer one for the engine's readback to
 * find. The one attribute a fan-write case names can drop writes (the
 * device answers, keeps its value) or refuse them (the bus fails). */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *drop_attr;       /* a path suffix, or NULL */
static int drop_left;               /* writes to drop; -1 = every one */
static int refuse;                  /* refuse instead of dropping */
static int drop_fd = -1;
/* The engine's latch hold as the kernel had it when the verdict was
   written: the order of the release and the verdict shows in it. */
static int verdict_fd = -1;
static long latch_at_verdict = -1;
/* The engine's hold as the kernel had it when the controller's latch was
   written: the order of the two locks shows in it. */
static int ctl_latch_fd = -1;
static long cooling_at_ctl_lock = -2;
/* The highest heater duty written since the case last zeroed it: a write
   the tick takes back before its end still shows. */
static int heater_fd = -1;
static long heater_written_max = 0;
/* Heater writes above zero that landed while the pump read off. */
static int heat_with_pump_off = 0;
/* The kernel pair's order: the turn of the last cnc/stop "1" and of the
   last cnc/laser_latch_cooling "1" among the writes since a case zeroed
   write_turn. */
static int stop_fd = -1, cool_lock_fd = -1;
static int write_turn, stop_turn, cool_lock_turn;
static long get_long(const char *attr);
/* Something done once, at the engine's next open of a path ending so: a
   report landing inside a tick, between its snapshot and its publish. */
static const char *on_open_suffix;
static void (*on_open_do)(void);

static int fake_open(const char *path, int flags, ...)
{
    int drop = 0;
    if (on_open_suffix) {
        size_t lp = strlen(path), ls = strlen(on_open_suffix);
        if (lp >= ls && !strcmp(path + lp - ls, on_open_suffix)) {
            on_open_suffix = NULL;
            on_open_do();
        }
    }
    if ((flags & O_WRONLY) && drop_attr && drop_left != 0) {
        size_t lp = strlen(path), la = strlen(drop_attr);
        drop = lp >= la && !strcmp(path + lp - la, drop_attr);
    }
    /* A dropped write leaves the device's value where it was: no
     * truncation for that one. */
    int fd = openat(AT_FDCWD, path, drop ? (flags & ~O_TRUNC) : flags, 0644);
    if (fd >= 0 && drop)
        drop_fd = fd;
    if (fd >= 0 && (flags & O_WRONLY) && strstr(path, ".cooling.state.tmp"))
        verdict_fd = fd;
    {
        size_t lp = strlen(path), la = strlen("cnc/laser_latch");
        if (fd >= 0 && (flags & O_WRONLY) && lp >= la && !strcmp(path + lp - la, "cnc/laser_latch"))
            ctl_latch_fd = fd;
        size_t lh = strlen("thermal/heater_pwm");
        if (fd >= 0 && (flags & O_WRONLY) && lp >= lh && !strcmp(path + lp - lh, "thermal/heater_pwm"))
            heater_fd = fd;
        size_t ls = strlen("cnc/stop"), lc = strlen("cnc/laser_latch_cooling");
        if (fd >= 0 && (flags & O_WRONLY) && lp >= ls && !strcmp(path + lp - ls, "cnc/stop"))
            stop_fd = fd;
        if (fd >= 0 && (flags & O_WRONLY) && lp >= lc && !strcmp(path + lp - lc, "cnc/laser_latch_cooling"))
            cool_lock_fd = fd;
    }
    return fd;
}

static ssize_t fake_write(int fd, const void *buf, size_t n)
{
    if (fd == verdict_fd) {
        verdict_fd = -1;
        latch_at_verdict = get_long("cnc/laser_latch_cooling");
    }
    if (fd == ctl_latch_fd) {
        ctl_latch_fd = -1;
        cooling_at_ctl_lock = get_long("cnc/laser_latch_cooling");
    }
    if (fd == heater_fd) {
        heater_fd = -1;
        char v[24];
        size_t k = n < sizeof(v) - 1 ? n : sizeof(v) - 1;
        memcpy(v, buf, k);
        v[k] = '\0';
        long d = strtol(v, NULL, 10);
        if (d > heater_written_max)
            heater_written_max = d;
        if (d > 0 && get_long("thermal/water_pump_on") == 0)
            heat_with_pump_off++;
    }
    if (fd == stop_fd || fd == cool_lock_fd) {
        if (n > 0 && ((const char *)buf)[0] == '1')
            *(fd == stop_fd ? &stop_turn : &cool_lock_turn) = ++write_turn;
        if (fd == stop_fd)
            stop_fd = -1;
        else
            cool_lock_fd = -1;
    }
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
/* The button's reservations on the same fake clock as the engine. */
#include "../src/btnres.c"

#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* --- fakes ------------------------------------------------------------ */

static double fake_now = 1000.0;
static double fake_step;            /* added to the clock after each read: time passing between two reads */

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
    fake_now += fake_step;
    return 0;
}

static const char *kv[16];          /* key, value, ..., NULL */
static int settings_unreadable;     /* the file there and not readable */

/* settings.c's answers: a value whole, too long for the buffer, absent,
   or the file unreadable. */
int settings_get(const char *key, char *val, size_t len)
{
    if (settings_unreadable)
        return SETTINGS_UNREADABLE;
    for (size_t i = 0; kv[i]; i += 2)
        if (!strcmp(kv[i], key)) {
            if (strlen(kv[i + 1]) >= len)
                return SETTINGS_TOO_LONG;
            snprintf(val, len, "%s", kv[i + 1]);
            return 0;
        }
    return SETTINGS_ABSENT;
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

static int log_errs;                /* lines at LOG_ERR, counted by level */
/* Called with each line as it is logged: a case that lands a call at
   that moment (another thread, on one core). */
static void (*fflog_hook)(const char *line);

void fflog(int prio, const char *fmt, ...)
{
    if (prio == LOG_ERR)
        log_errs++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    size_t n = strlen(logbuf);
    if (n + strlen(last_log) + 2 < sizeof(logbuf))
        snprintf(logbuf + n, sizeof(logbuf) - n, "%s\n", last_log);
    printf("    log: %s\n", last_log);
    if (fflog_hook)
        fflog_hook(last_log);
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
   answering). An arm needs the handle a successful open gave; an open
   that fails leaves no handle (the part does not answer), and an arm can
   be made to fail on a handle that is there. */
static int crash_armed_fake;
static int crash_arm_calls;
static int crash_poll_fail;
static int crash_handle_fake;
static int crash_open_fail, crash_arm_fail;
static int crash_open_calls;
static unsigned crash_src1, crash_src2;
int fake_crash_open(void)
{
    crash_open_calls++;
    crash_handle_fake = !crash_open_fail;
    return crash_handle_fake ? 0 : -1;
}
int fake_crash_arm(int x, int y, int abort)
{
    (void)x; (void)y; (void)abort;
    if (!crash_handle_fake || crash_arm_fail)
        return -1;
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
void fake_crash_close(void) { crash_handle_fake = 0; }

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
    stop_seen_latch = get_long("cnc/laser_latch_cooling");
}
static int machine_idle = 1;        /* the kernel idle, as status.c reads it */
int machine_is_idle(void) { return machine_idle; }
int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    (void)t; (void)a; (void)fn; (void)arg;
    return 0;
}
static int joins;
static long tec_at_join = -1, pump_at_join = -1;
static int join_writes_tec;             /* the joined tick writes the TEC on and the pump off
                                           (a pump hold it applied; shutdown_order) */
static void put_long(const char *attr, long v);
int fake_pthread_join(pthread_t t, void **ret)
{
    (void)t; (void)ret;
    joins++;
    tec_at_join = get_long("thermal/tec_on");
    pump_at_join = get_long("thermal/water_pump_on");
    if (join_writes_tec) {
        put_long("thermal/tec_on", 1);
        put_long("thermal/water_pump_on", 0);
    }
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
/* A tick placed inside a quiet hold's call, where its yield falls (a
   forced schedule): one engine tick of a run session opening. */
static int yield_ticks_session;
static void yield_tick(void);
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
    if (yield_ticks_session) {
        yield_ticks_session = 0;
        yield_tick();
    }
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
/* A count the case has read as exactly degc_exact_c (the 70 C line's own
 * edge, which no count converts to), or -1. */
static long degc_exact_raw = -1;
static double degc_exact_c;

double coolant_degc(long raw)
{
    static const double adc_f = 1024.0 * 1.3;
    if (raw == degc_exact_raw)
        return degc_exact_c;
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
    put("cnc/laser_latch_cooling", "1\n");  /* every holder holds after a final close */
    put("cnc/laser_latch", "1\n");
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
    /* The lid IR channels at a lit lid's baseline: a channel that does not
     * read is the fire watch's SENSOR. */
    for (int i = 1; i <= 4; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    /* The fan duties the engine writes and reads back. */
    put_long("head/air_assist_pwm", 204);
    put_long("thermal/exhaust_pwm", 0);
    put_long("thermal/intake_pwm", 0);
    put_long("thermal/tec_on", 0);
    put_long("thermal/water_pump_on", 1);
}

/* --- the loop model --------------------------------------------------- */

static const double BASE_C = 22.3;

typedef struct {
    int dead_down, dead_up;         /* a sensor whose read fails */
    int session;                    /* ticks since the session opened */
    double bulk_c;                  /* the bulk warmed past its base (a warm-up) */
    double down_extra_c;            /* downstream over upstream (the loop heater's own heat) */
    long fix_down, fix_up;          /* a sensor that reads this count (> -1), whatever the loop */
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
        put_long("pic/water_temp_1", L->fix_down > -1 ? L->fix_down : raw_for(down));
    if (L->dead_up)
        unlink(GF_SYSFS "pic/water_temp_2");
    else
        put_long("pic/water_temp_2", L->fix_up > -1 ? L->fix_up : raw_for(up));
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
   through the cooldown phases until the engine is idle. The heater is the
   engine's to take off (gates B). */
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

/* One tick of an open run session whose window has closed. */
static void tick_unarmed_run(loop_t *L)
{
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 0, -1, -1, -1, NULL);
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

static int now_in_grace(void)
{
    return wall_s() < fan_grace_until;
}

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

    /* Once given, it goes only with a fan that stays under its floor for
     * the airflow gate's count of ticks: one low reading does not take
     * it. The fan's spin-up grace stops nothing here: the session below
     * is inside it throughout, where the airflow gate judges nothing. The
     * window owes its fans from its acknowledgment, so the withdrawal
     * blocks fire and holds the window (FANS) inside the grace, where
     * AIRFLOW cannot trip yet, until the fan reads its floor again. */
    printf("L2. one low tach reading keeps the acknowledgment; the gate's count withdraws it and holds "
           "the window, in the session's grace too\n");
    settle_and_open(L);
    CHECK(pub_armed_ack && now_in_grace(), "a fresh session, every fan fast: acknowledged, in the grace");
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 1; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(pub_armed_ack && pub_fire_ok, "under its floor for fewer ticks than the gate's count: still acknowledged");
    run_tick(L);
    CHECK(!pub_armed_ack && now_in_grace() && !airflow_alarm, "under its floor for the gate's count: withdrawn, inside the grace");
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "FANS"),
          "the acknowledged window with a fan gone: FANS, fire blocked, the window held");
    /* FANS goes only once every judged fan has read its floor for the
     * gate's count of ticks in a row: a fan that dips and comes back for a
     * tick does not cycle the job lit and dark. */
    put_long("thermal/tach_exhaust", 300);
    for (int i = 1; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(pub_armed_ack && !pub_fire_ok && pub_hold && !strcmp(pub_verdict, "FANS"),
          "back at its floor for fewer ticks than the count: acknowledged, FANS still holds");
    put_long("thermal/tach_exhaust", 10000000);
    run_tick(L);
    put_long("thermal/tach_exhaust", 300);
    for (int i = 1; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "FANS"), "a dip in between starts the count again");
    run_tick(L);
    CHECK(pub_armed_ack && pub_fire_ok && !pub_hold,
          "at its floor for the count of ticks in a row: fire permitted, released");
    /* A window that has closed holds nothing, whatever it owed. */
    tick_unarmed_run(L);
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS + 1; i++)
        tick_unarmed_run(L);
    CHECK(strcmp(pub_verdict, "FANS") && !pub_hold, "the window closed, a fan under its floor: no FANS, nothing held");
    put_long("thermal/tach_exhaust", 300);
    close_session(L);

    /* A restart of the daemon under a busy job: the new engine hears of a
     * window it never saw closed, which may have fired, so the window owes
     * its fans from the engine's first tick, and fire waits for them
     * (FANS) instead of a permissive verdict with the acknowledgment
     * withheld. */
    printf("L2b. a window open before the engine started owes its fans: FANS until they read their floors\n");
    settle_and_open(L);
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack, "an armed run, acknowledged");
    put_long("thermal/tach_exhaust", 10000000);     /* the exhaust stops */
    machine_idle = 0;
    cool_init();                        /* the daemon starts again under the busy job */
    /* The engine ticks at once, before its server takes the controller's
     * first report: a tick with no report is not a window seen closed. */
    loop_tick(L);
    fake_now += 1.0;
    engine_tick();
    run_tick(L);                        /* the controller's first report to it: its window open */
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "FANS") && !airflow_alarm && now_in_grace(),
          "the window from before the start, the exhaust stopped: FANS in the new grace, fire blocked, held");
    put_long("thermal/tach_exhaust", 300);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(pub_fire_ok && !pub_hold && pub_armed_ack,
          "the exhaust at its floor for the count of ticks: fire permitted, released, acknowledged");
    machine_idle = 1;
    close_session(L);

    /* The acknowledgment is the window's own. An unarmed run stretch whose
     * job holds the exhaust under its run duty does not judge it, so the
     * airflow reads up; the window opening raises the exhaust to its run
     * duty and judges it, and a fan still under its floor then holds the
     * acknowledgment back from the window's first tick. */
    printf("L3. a window opening is acknowledged on its own fans, never on the stretch before it\n");
    settle_and_open(L);
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < 3; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("run", 0, -1, 20000, -1, NULL);
        engine_tick();
    }
    CHECK(fans_up && !pub_armed_ack, "unarmed, the slowed exhaust unjudged: the airflow reads up, nothing armed");
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, 20000, -1, NULL);
    engine_tick();
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN && !pub_armed_ack,
          "the window opens: the exhaust raised to its run duty, the window not acknowledged");
    put_long("thermal/tach_exhaust", 300);
    run_tick(L);
    CHECK(pub_armed_ack, "the exhaust at its floor: acknowledged");
    close_session(L);
}

static int verdict_has(const char *needle);

/* FANS is a pause tier. A verdict that ends the job or waits for the
 * operator stands over it by name, since the controllers' fail tier, the
 * red flash and its acknowledgment, the flight recorder and the cloud
 * client's fault key on the name; and the words stay the verdict's: one
 * that rises with FANS keeps its own, and the ceiling's hold under FANS
 * names itself again once FANS goes. */
static void fans_under_the_fail_tiers(loop_t *L)
{
    printf("L2c. CRITICAL, SENSOR and AIRFLOW name themselves over FANS, with their own words\n");
    attn_keep = 1;
    close_session(L);
    (void)cool_attention_ack();
    kv[0] = "cool_temp_max"; kv[1] = "60"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_temp_critical_c"; kv[5] = "30"; kv[6] = NULL;
    settle_and_open(L);
    run_tick(L);
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "FANS") && now_in_grace() && !attn_latched,
          "an acknowledged window, the exhaust under its floor in the grace: FANS, nothing for the operator");
    L->bulk_c = 10.0;                   /* 32.3 C: over the 30 C critical line */
    run_tick(L);
    CHECK(critical_alarm && !strcmp(pub_verdict, "CRITICAL") && attn_latched && attn_sticky && led_red &&
          strstr(pub_reason, "CRITICAL"),
          "the critical line crossed under FANS: CRITICAL by name and words, red, its acknowledgment taken");
    /* The job canceled: the session ends, and CRITICAL's acknowledgment waits. */
    L->bulk_c = 0.0;
    put_long("thermal/tach_exhaust", 300);
    close_session(L);
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "ACK") && !pub_fire_ok && verdict_has("\"ack_wait\":true"),
          "the job canceled: CRITICAL's acknowledgment waits");
    (void)cool_attention_ack();

    settle_and_open(L);
    run_tick(L);
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    L->dead_up = 1;
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && attn_latched && led_red && strstr(pub_reason, "SENSOR"),
          "a coolant sensor gone under FANS: SENSOR by name and words, red");
    L->dead_up = 0;
    put_long("thermal/tach_exhaust", 300);
    close_session(L);
    (void)cool_attention_ack();
    run_idle_tick(L);

    /* Past the grace, a fan that stops trips AIRFLOW in the tick FANS rises. */
    settle_and_open(L);
    {
        double t0 = fake_now;
        while (fake_now < t0 + FAN_GRACE_S + 1)
            run_tick(L);
    }
    CHECK(pub_armed_ack && pub_fire_ok, "an acknowledged window past the grace");
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(airflow_alarm && fans_held && !strcmp(pub_verdict, "AIRFLOW") && strstr(pub_reason, "AIRFLOW") &&
          !strcmp(attn_reason, pub_reason),
          "AIRFLOW and FANS in one tick: AIRFLOW by name, its words published and on its acknowledgment");
    put_long("thermal/tach_exhaust", 300);
    close_session(L);
    (void)cool_attention_ack();
    run_idle_tick(L);

    /* The ceiling's hold under FANS names itself again once FANS goes. */
    kv[1] = "18";
    settle_and_open(L);
    run_tick(L);
    put_long("thermal/tach_exhaust", 10000000);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(over_temp_gate && !strcmp(pub_verdict, "FANS"), "the ceiling's hold under FANS");
    put_long("thermal/tach_exhaust", 300);
    for (int i = 0; i < AIRFLOW_DEBOUNCE_TICKS; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && strstr(pub_reason, "limit"),
          "FANS gone: OVERTEMP with its own words");
    kv[0] = NULL;
    attn_keep = 0;
    close_session(L);
}

/* A quiet hold (every fan off for a listening) taken inside an armed
 * window blocks fire at the next tick and holds the window, whatever let
 * it in; outside a window it blocks fire and holds nothing. */
static void quiet_blocks_fire(loop_t *L)
{
    printf("L4. a quiet hold blocks fire at the next tick, and holds only an open window\n");
    open_session(L);
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack, "an armed run, acknowledged");
    cool_quiet_hold_ex(1, 0);
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "QUIET"),
          "the quiet hold in the window: QUIET, fire blocked, the window held");
    close_session(L);
    run_idle_tick(L);
    CHECK(!pub_fire_ok && !pub_hold, "the quiet hold at idle: fire blocked, nothing held");
    cool_quiet_hold(0);
    run_idle_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "released: OK");
}

/* The machine's button reserved for a prompt (btnres.c): fire blocked, the
 * window not acknowledged and held, the reservation in the document, until
 * it runs out. A package's own reservation does not withhold its own job
 * while it keeps the machine, and withholds every job while it does not
 * (a job sent in its name from elsewhere then waits like any other). */
static int own_keeps;
static int fake_pkg_holds(const char *pkg)
{
    return own_keeps && !strcmp(pkg, "org.example.own");
}

static void button_reserved_case(loop_t *L)
{
    char why[96];
    printf("L5. the button reserved for a prompt withholds the window and blocks fire until it ends\n");
    cool_button_reserved = btnres_withheld;
    btnres_pkg_holds = fake_pkg_holds;
    close_session(L);
    CHECK(btnres_open("panel:a1", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "the panel reserves the button");
    /* The time on file counts from a document a reader can have read: a
     * publish that does not land (its rename refused: a directory stands
     * where the file goes) puts nothing on file. */
    unlink(VERDICT_FILE);
    CHECK(mkdir(VERDICT_FILE, 0755) == 0, "the verdict file's place taken by a directory");
    run_idle_tick(L);
    fake_now += BTNRES_READY_S;
    CHECK(!strcmp(pub_verdict, "BUTTON") && cool_button_on_file_s() < 0,
          "BUTTON with no document landed: nothing on file");
    CHECK(rmdir(VERDICT_FILE) == 0, "and given back");
    run_idle_tick(L);
    CHECK(!pub_fire_ok && !pub_hold && !strcmp(pub_verdict, "BUTTON") && verdict_has("\"button_reserved\":true"),
          "idle: BUTTON in the document, fire blocked, nothing held");
    fake_now += BTNRES_READY_S;
    CHECK(cool_button_on_file_s() >= BTNRES_READY_S, "on file since that publish");
    L->session = 1;
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && !pub_armed_ack && fans_up,
          "an armed window under it, its fans up: fire blocked, held, not acknowledged");
    fake_now += BTNRES_PANEL_TTL_S;
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack && !verdict_has("\"button_reserved\":true") && cool_button_on_file_s() < 0,
          "the reservation ran out: fire permitted, acknowledged, nothing reserved");
    /* A renewal moves the reservation's end; each prompt's is its own, and
     * one ended ends no other. */
    CHECK(btnres_open("panel:a1", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0 &&
          btnres_open("panel:b2", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "two dialogs reserve the button");
    fake_now += BTNRES_PANEL_TTL_S - 2.0;
    CHECK(btnres_open("panel:a1", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "one renews");
    btnres_close("panel:b2");
    fake_now += BTNRES_PANEL_TTL_S - 2.0;
    run_tick(L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "BUTTON"),
          "past the first end, the renewed one stands, the other's close ending only its own");
    btnres_close("panel:a1");
    run_tick(L);
    CHECK(pub_fire_ok, "closed: nothing reserved");

    CHECK(btnres_open("check:org.example.own", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "a package's check reserves it");
    run_tick(L);
    CHECK(!pub_fire_ok && !pub_armed_ack, "the package does not keep the machine: the window withheld");
    own_keeps = 1;
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack, "the package keeps the machine: its own window acknowledged");
    CHECK(btnres_open("check:org.example.other", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "another package's check beside it");
    run_tick(L);
    CHECK(!pub_fire_ok && !pub_armed_ack, "another package's check: the window withheld");
    btnres_close("check:org.example.own");
    btnres_close("check:org.example.other");
    /* A package's ask for a press withholds every job, its own included,
     * though it keeps the machine: the press it is handed arms nothing. */
    const char *ask_state = "";
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack, "with the checks closed the window is acknowledged again");
    CHECK(btnres_ask("org.example.own", &ask_state, why, sizeof(why)) == 0, "the package asks for a press");
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && !pub_armed_ack && verdict_has("\"button_reserved\":true"),
          "its ask, the package keeping the machine: its own window withheld and held");
    btnres_ask_end("org.example.own");
    run_tick(L);
    CHECK(pub_fire_ok && pub_armed_ack, "the ask withdrawn: acknowledged again");
    own_keeps = 0;
    close_session(L);
    cool_button_reserved = NULL;
    btnres_pkg_holds = NULL;
}

/* A package's group under the extension host's directory, as the kernel
 * shows it: its events and its cpu.max. */
static void ext_group(const char *grp, const char *events, const char *cpu_max)
{
    char p[160];
    snprintf(p, sizeof(p), EXT_CG_DIR "/%s", grp);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), EXT_CG_DIR "/%s/cgroup.events", grp);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(events, f);
        fclose(f);
    }
    snprintf(p, sizeof(p), EXT_CG_DIR "/%s/cpu.max", grp);
    if ((f = fopen(p, "w"))) {
        fputs(cpu_max, f);
        fclose(f);
    }
}

/* The window's acknowledgment waits for the packages (forgeext freezes
 * them at its next turn, which can come after the first fire could): not
 * while a package's group holds a process that is neither frozen nor held
 * to the job's share of the core, nor while a group does not read; once
 * each is still, and with no packages at all. */
static void packages_case(loop_t *L)
{
    printf("L6. the window's acknowledgment waits for every package to be still\n");
    close_session(L);
    settle_and_open(L);
    run_tick(L);
    CHECK(pub_armed_ack, "no packages' directory: acknowledged");
    close_session(L);
    mkdir(EXT_CG_DIR, 0755);
    FILE *f = fopen(EXT_CG_DIR "/cgroup.procs", "w");     /* one of the parent's own files */
    if (f)
        fclose(f);
    ext_group("org.example.a", "populated 1\nfrozen 0\n", "25000 100000\n");
    settle_and_open(L);
    CHECK(!pub_armed_ack && fans_up && strstr(pub_reason, "org.example.a"),
          "the window's first tick, a package not still: not acknowledged, its fans up, the reason names it");
    for (int i = 0; i < 5; i++)
        run_tick(L);
    CHECK(!pub_armed_ack && fans_up && pub_fire_ok,
          "a package running at its own share, not frozen: the window not acknowledged, its fans up");
    close_session(L);
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    settle_and_open(L);
    for (int i = 0; i < 3; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && !pub_armed_ack && !strstr(pub_reason, EXT_WAIT_WORDS),
          "another verdict standing, a package not still: the reason is that verdict's own");
    kv[0] = NULL;
    close_session(L);
    settle_and_open(L);
    for (int i = 0; i < 5; i++)
        run_tick(L);
    ext_group("org.example.a", "populated 1\nfrozen 1\n", "25000 100000\n");
    ext_group("org.example.granted", "populated 1\nfrozen 0\n", "3000 100000\n");
    ext_group("org.example.gone", "populated 0\nfrozen 0\n", "max 100000\n");
    run_tick(L);
    CHECK(pub_armed_ack, "frozen, held to the job's share, or empty: acknowledged");
    ext_group("org.example.gone", "populated 1\nfrozen 0\n", "max 100000\n");
    run_tick(L);
    CHECK(!pub_armed_ack, "a group with no limit, not frozen: not acknowledged");
    ext_group("org.example.gone", "populated 0\nfrozen 0\n", "max 100000\n");
    unlink(EXT_CG_DIR "/org.example.a/cgroup.events");
    mkdir(EXT_CG_DIR "/org.example.a/cgroup.events", 0755);  /* there, and not readable */
    run_tick(L);
    CHECK(!pub_armed_ack, "a group's events that do not read: not acknowledged");
    rmdir(EXT_CG_DIR "/org.example.a/cgroup.events");
    ext_group("org.example.a", "populated 1\nfrozen 1\n", "25000 100000\n");
    run_tick(L);
    CHECK(pub_armed_ack && !strstr(pub_reason, "PACKAGES"), "readable again, frozen: acknowledged, the reason gone");
    /* Whatever does not read as still keeps the window waiting, each with
     * every other group still. (A readdir error cannot be made in a tree of
     * plain files.) */
    ext_group("org.example.b", "frozen 0\n", "25000 100000\n");
    run_tick(L);
    CHECK(!pub_armed_ack, "a group's events with no populated key: not acknowledged");
    ext_group("org.example.b", "populated 1\n", "25000 100000\n");
    run_tick(L);
    CHECK(!pub_armed_ack, "a group's events with no frozen key, at its own share: not acknowledged");
    ext_group("org.example.b", "populated 1\nfrozen 0\n", "3001 100000\n");
    run_tick(L);
    CHECK(!pub_armed_ack, "a group held to just over the job's share: not acknowledged");
    ext_group("org.example.b", "populated 1\nfrozen 0\n", "max 100000\n");
    unlink(EXT_CG_DIR "/org.example.b/cpu.max");
    mkdir(EXT_CG_DIR "/org.example.b/cpu.max", 0755);      /* there, and not readable */
    run_tick(L);
    CHECK(!pub_armed_ack, "a group whose cpu.max does not read: not acknowledged");
    rmdir(EXT_CG_DIR "/org.example.b/cpu.max");
    ext_group("org.example.b", "populated 0\nfrozen 0\n", "max 100000\n");
    run_tick(L);
    CHECK(pub_armed_ack, "that group empty: acknowledged");
    CHECK(symlink("org.example.loop", EXT_CG_DIR "/org.example.loop") == 0, "an entry that does not stat (a loop)");
    run_tick(L);
    CHECK(!pub_armed_ack, "an entry whose stat fails: not acknowledged");
    unlink(EXT_CG_DIR "/org.example.loop");
    close_session(L);
    const char *grps[] = { "org.example.a", "org.example.b", "org.example.granted", "org.example.gone" };
    char p[160];
    for (size_t i = 0; i < sizeof(grps) / sizeof(grps[0]); i++) {
        snprintf(p, sizeof(p), EXT_CG_DIR "/%s/cgroup.events", grps[i]);
        unlink(p);
        snprintf(p, sizeof(p), EXT_CG_DIR "/%s/cpu.max", grps[i]);
        unlink(p);
        snprintf(p, sizeof(p), EXT_CG_DIR "/%s", grps[i]);
        rmdir(p);
    }
    unlink(EXT_CG_DIR "/cgroup.procs");
    rmdir(EXT_CG_DIR);
    if ((f = fopen(EXT_CG_DIR, "w")))                       /* the directory of groups that does not open */
        fclose(f);
    settle_and_open(L);
    run_tick(L);
    CHECK(!pub_armed_ack && fans_up, "a directory of groups that does not open: not acknowledged");
    close_session(L);
    unlink(EXT_CG_DIR);
}

/* The extension host's status file, as it writes it: its pid, whether it
 * runs nothing (quiet), the window it has paused its packages for. */
static void host_word(long long pid, int quiet, unsigned long long window)
{
    FILE *f = fopen(EXT_HOST_STATUS, "w");
    if (f) {
        fprintf(f, "{\"pid\": %lld, \"quiet\": %s, \"window\": %llu, \"services\": []}\n", pid,
                quiet ? "true" : "false", window);
        fclose(f);
    }
}

/* A status file of another shape, whole. */
static void host_status_text(const char *text)
{
    FILE *f = fopen(EXT_HOST_STATUS, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* The host's word for this window is part of the acknowledgment: a turn
 * that read the window closed can start or thaw a service after the window
 * is reported, which the kernel cannot see coming. No package's group
 * exists in these ticks, so the kernel reads every one still. */
static void host_word_case(loop_t *L)
{
    printf("L7. the window's acknowledgment waits for the extension host's word for this window\n");
    close_session(L);
    mkdir("cool-engine-test/ext", 0755);
    settle_and_open(L);
    host_word(getpid(), 0, 0);
    run_tick(L);
    CHECK(!pub_armed_ack && fans_up && strstr(pub_reason, "extension host"),
          "a running host that has not named this window: not acknowledged, the reason says why");
    host_word(getpid(), 0, rep_window - 1);
    run_tick(L);
    CHECK(!pub_armed_ack, "a running host that names another window: not acknowledged");
    host_word(getpid(), 0, rep_window);
    run_tick(L);
    CHECK(pub_armed_ack, "a running host that names this window: acknowledged");
    host_word(getpid(), 1, 0);
    run_tick(L);
    CHECK(pub_armed_ack, "a running host that runs nothing and starts nothing (quiet): acknowledged");
    char text[160];
    snprintf(text, sizeof(text), "{\"pid\": %lld, \"services\": []}\n", (long long)getpid());
    host_status_text(text);                                 /* as a host older than this engine writes it */
    run_tick(L);
    CHECK(!pub_armed_ack && strstr(pub_reason, "older"),
          "a running host whose status has no quiet (an older host): not acknowledged, the reason says so");
    snprintf(text, sizeof(text), "{\"pid\": %lld, \"quiet\": false, \"services\": []}\n", (long long)getpid());
    host_status_text(text);
    run_tick(L);
    CHECK(!pub_armed_ack, "a running host whose status names no window at all: not acknowledged");
    snprintf(text, sizeof(text), "{\"quiet\": true, \"window\": %llu, \"services\": []}\n", rep_window);
    host_status_text(text);
    run_tick(L);
    CHECK(!pub_armed_ack, "a status with no pid, quiet and naming this window: not acknowledged");
    pid_t gone = fork();
    if (gone == 0)
        _exit(0);
    waitpid(gone, NULL, 0);
    host_word(gone, 0, 0);
    run_tick(L);
    CHECK(pub_armed_ack, "a status whose writer is gone: acknowledged");
    snprintf(text, sizeof(text), "{\"pid\": %lld, \"services\": []}\n", (long long)gone);
    host_status_text(text);                                 /* an older host's, its writer gone */
    run_tick(L);
    CHECK(!pub_armed_ack && strstr(pub_reason, "older"),
          "a status with no quiet whose writer is gone (an older host): not acknowledged, the reason says so");
    unlink(EXT_HOST_STATUS);
    mkdir(EXT_HOST_STATUS, 0755);                           /* there, and not readable */
    run_tick(L);
    CHECK(!pub_armed_ack, "a status that does not read: not acknowledged");
    rmdir(EXT_HOST_STATUS);
    run_tick(L);
    CHECK(pub_armed_ack, "no status file (no host has run): acknowledged");
    rmdir("cool-engine-test/ext");
    FILE *f = fopen("cool-engine-test/ext", "w");           /* the file's directory a file: its open fails */
    if (f)
        fclose(f);
    run_tick(L);
    CHECK(!pub_armed_ack, "a status whose open fails for another reason than its absence: not acknowledged");
    unlink("cool-engine-test/ext");
    mkdir("cool-engine-test/ext", 0755);
    close_session(L);
}

/* A report landing inside a tick: the window closes, and a new one opens,
 * between the tick's snapshot and its publish. */
static void close_and_reopen(void)
{
    cool_state_report("run", 0, -1, -1, -1, NULL);
    cool_state_report("run", 1, -1, -1, -1, NULL);
}

static void close_only(void)
{
    cool_state_report("run", 0, -1, -1, -1, NULL);
}

/* The acknowledgment is the window's own: off the file the moment its
 * window's close is reported (or it is shown closed), never put back by a
 * tick that judged the window before, and a close and an opening between
 * two ticks make a new window, judged afresh. */
static void window_own_ack_case(loop_t *L)
{
    char doc[COOL_STATUS_JSON_MAX], want[48];
    printf("L8. the acknowledgment is the window's own\n");
    close_session(L);
    settle_and_open(L);
    run_tick(L);
    cool_status_json(doc, sizeof(doc));
    snprintf(want, sizeof(want), "\"window\":%llu,", rep_window);
    CHECK(pub_armed_ack && rep_window > 0 && strstr(doc, want), "acknowledged, /cool/status names the window");
    cool_state_report("run", 0, -1, -1, -1, NULL);
    CHECK(verdict_has("\"armed\":false"), "the window's close reported: the acknowledgment off the file before any tick");
    run_tick(L);
    run_tick(L);
    CHECK(pub_armed_ack, "a new window, acknowledged");
    put_long("thermal/tach_exhaust", 10000000);             /* one low reading: an open window keeps its fans up */
    loop_tick(L);
    fake_now += 1.0;
    close_and_reopen();
    engine_tick();
    CHECK(!pub_armed_ack && !fans_up, "a close and an opening between two ticks: a new window, its fans judged afresh");
    put_long("thermal/tach_exhaust", 300);
    run_tick(L);
    CHECK(pub_armed_ack, "its fans at their floors: acknowledged");
    on_open_suffix = EXT_HOST_STATUS;                       /* opened after the tick's snapshot, before its publish */
    on_open_do = close_and_reopen;
    run_tick(L);
    CHECK(!pub_armed_ack && verdict_has("\"armed\":false"),
          "a close and an opening reported inside a tick: that tick's document does not carry the acknowledgment");
    run_tick(L);
    CHECK(pub_armed_ack, "the next tick judges the new window: acknowledged");
    on_open_suffix = EXT_HOST_STATUS;
    on_open_do = close_only;
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
    CHECK(!pub_armed_ack && verdict_has("\"armed\":false"),
          "a close reported inside a tick: that tick's document does not carry the acknowledgment");
    run_tick(L);
    run_tick(L);
    CHECK(pub_armed_ack, "opened again: acknowledged");
    unsigned long long before = rep_window;
    host_word(getpid(), 0, before);
    run_tick(L);
    CHECK(pub_armed_ack, "the host names it: acknowledged");
    fake_now = rep_at + REPORT_TIMEOUT_S - 0.0005;          /* the report half a ms from stale, */
    fake_step = 0.001;                                      /* and a ms passing at every read of the clock */
    cool_status_json(doc, sizeof(doc));
    fake_step = 0.0;
    CHECK(!(strstr(doc, "\"armed\":false") && verdict_has("\"armed\":true")),
          "a window /cool/status shows closed at the stale boundary: the acknowledgment off the file too");
    fake_now += REPORT_TIMEOUT_S + 1.0;                     /* the controller silent: its report goes stale */
    cool_status_json(doc, sizeof(doc));
    CHECK(strstr(doc, "\"armed\":false") && verdict_has("\"armed\":false"),
          "a window /cool/status shows closed (a stale report): the acknowledgment off the file first");
    run_tick(L);
    run_tick(L);
    CHECK(!pub_armed_ack && rep_window != before,
          "reported again after the stale stretch: a new window, the host's word for the last one does not count");
    host_word(getpid(), 0, rep_window);
    run_tick(L);
    CHECK(pub_armed_ack, "the host names the new one: acknowledged");
    fake_now += REPORT_TIMEOUT_S + 1.0;                     /* silent again, and nobody reads /cool/status */
    cool_state_report("run", 1, -1, -1, -1, NULL);
    CHECK(verdict_has("\"armed\":false"),
          "a report that opens a window after a stale stretch: the acknowledgment off the file before any tick");
    host_word(getpid(), 0, rep_window);
    run_tick(L);
    run_tick(L);
    CHECK(pub_armed_ack, "the host names that one: acknowledged");
    unlink(EXT_HOST_STATUS);
    cool_controller_stopped();
    CHECK(verdict_has("\"armed\":false"), "the controller stopped: the acknowledgment off the file");
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
    /* The window was acknowledged before the loss and owes its fans: the
     * restarted grace keeps AIRFLOW from tripping, and the fan under its
     * floor holds the window (FANS) meanwhile. */
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "FANS"),
          "under its floor in the restarted grace: FANS, fire blocked, the window held");
    put_long("head/air_assist_tach", AA_FAST);
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no fault once the fan is back at speed");
    CHECK(pub_fire_ok && !pub_hold, "and fire permitted again, the window released");
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

    printf("diagnostics B. a tool's fans are taken back at the release: the phase's duties\n");
    CHECK(cool_diag_take() == 0, "a diagnostic takes the thermal hardware");
    cool_diag_fans_run();
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN, "the tool runs the fans");
    cool_diag_release();
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_IDLE && get_long("thermal/intake_pwm") == INTAKE_IDLE,
          "the release writes the idle duties");
    run_idle_tick(L);
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_IDLE, "and they hold");
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

    printf("phases C. a busy start's end under OVERTEMP keeps the cooling airflow\n");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    machine_idle = 0;
    cool_init();
    for (int i = 0; i < 2; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && forced_cool, "the ceiling under the loop at idle: OVERTEMP's cooling airflow");
    machine_idle = 1;
    loop_tick(L);
    tick("idle", 0);
    CHECK(!start_cool && forced_cool && get_long("thermal/exhaust_pwm") == EXHAUST_COOL &&
          get_long("thermal/intake_pwm") == INTAKE_COOL,
          "the machine idle: the busy start ends, and the cooling airflow stands");
    kv[0] = NULL;
    conf_reload();
    loop_tick(L);
    tick("idle", 0);
    CHECK(!forced_cool && get_long("thermal/exhaust_pwm") == EXHAUST_IDLE, "the ceiling back over the loop: the idle duties");

    printf("phases D. a smoke phase that ends into idle with the gate still standing (the upstream reading lost "
           "in the smoke clear) lands on the cooling airflow\n");
    close_session(L);
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    settle_and_open(L);
    for (int i = 0; i < 3; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP"), "OVERTEMP in the run");
    L->dead_up = 1;
    L->session = 0;
    for (int i = 0; i < 40 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    CHECK(cool_state == Cool_Idle && over_temp_gate && get_long("thermal/exhaust_pwm") == EXHAUST_COOL &&
          get_long("thermal/intake_pwm") == INTAKE_COOL, "idle, the gate standing: the cooling airflow");
    L->dead_up = 0;
    kv[0] = NULL;
    L->bulk_c = -10.0;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    L->bulk_c = 0.0;
    conf_reload();
    close_session(L);
}

/* A diagnostic's take, landed as the tick that raises OVERTEMP logs its
 * rise: after the rise and before the tick publishes, as an HTTP thread
 * can land it on one core (warn() drops the lock before it logs). */
static int take_on_overtemp;
static int took_in_tick = -1;
static void take_on_rise(const char *line)
{
    if (take_on_overtemp && strstr(line, "limit - hold until")) {
        take_on_overtemp = 0;
        took_in_tick = cool_diag_take() == 0;
    }
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

    printf("gates A2. OVERTEMP's cooling airflow is a state: past a thermal phase that times out, under a quiet "
           "hold's release, with the TEC on only in airflow, no diagnostic or pump hold, gone with the recovery\n");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_cooldown_max_s"; kv[5] = "5"; kv[6] = "cool_tec_present"; kv[7] = "1";
    kv[8] = "cool_tec_on_c"; kv[9] = "15"; kv[10] = "cool_tec_off_c"; kv[11] = "13"; kv[12] = NULL;
    settle_and_open(L);
    for (int i = 0; i < 3; i++)
        run_tick(L);
    CHECK(!strcmp(pub_verdict, "OVERTEMP"), "the ceiling under the loop: OVERTEMP in the run");
    int tec_without_fans = 0;
    session_end_tick(L);
    for (int i = 0; i < 120 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        tick("idle", 0);
        if (get_long("thermal/tec_on") == 1 && get_long("thermal/exhaust_pwm") == 0)
            tec_without_fans = 1;
    }
    CHECK(cool_state == Cool_Idle && !strcmp(pub_verdict, "OVERTEMP") && forced_cool &&
          get_long("thermal/exhaust_pwm") == EXHAUST_COOL && get_long("thermal/intake_pwm") == INTAKE_COOL,
          "the thermal phase timed out over the gate: idle, OVERTEMP, the cooldown duties stand");
    for (int i = 0; i < 30; i++) {
        run_idle_tick(L);
        if (get_long("thermal/tec_on") == 1 && get_long("thermal/exhaust_pwm") == 0)
            tec_without_fans = 1;
    }
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_COOL, "and still 30 s later");
    CHECK(!tec_without_fans, "the TEC never on with the exhaust at 0");
    int take = cool_diag_take(), took = take == 0;
    if (took)
        cool_diag_release();
    CHECK(take == -2 && cool_quiet_hold_try(1, 1) == -3 && cool_quiet_hold_try(1, 0) == -3 && !quiet_held,
          "no diagnostic (refused as the over-temp cooldown, not a busy engine), and the route refuses a "
          "hold, with the pump or fans only, while it stands");
    cool_quiet_hold_ex(1, 1);           /* set past the route's check */
    run_idle_tick(L);
    CHECK(!quiet_held && !quiet_pump_held && get_long("thermal/water_pump_on") == 1 && !quiet_pump_applied,
          "a pump hold set past the route's check: released at the engine's next tick, the pump on");
    for (int i = 0; i < 60 && get_long("thermal/tec_on") != 1; i++)
        run_idle_tick(L);
    cool_quiet_hold(1);
    run_idle_tick(L);
    CHECK(!quiet_held && get_long("thermal/exhaust_pwm") == EXHAUST_COOL && forced_cool &&
          get_long("thermal/tec_on") == 1,
          "a fans-only hold set under it: released at the engine's next tick, the cooling airflow and the TEC "
          "stand"); 
    L->session = 1;
    run_tick(L);
    CHECK(!forced_cool && get_long("thermal/exhaust_pwm") == EXHAUST_RUN,
          "a session that opens over it: the run duties, and the cooling airflow's flag down");
    session_end_tick(L);
    for (int i = 0; i < 120 && cool_state != Cool_Idle; i++) {
        loop_tick(L);
        tick("idle", 0);
    }
    CHECK(forced_cool && get_long("thermal/exhaust_pwm") == EXHAUST_COOL, "and at idle again, the cooling airflow");
    L->bulk_c = -7.0;                   /* 15.3 C: under the resume gate */
    run_idle_tick(L);
    CHECK(strcmp(pub_verdict, "OVERTEMP") && !forced_cool &&
          get_long("thermal/exhaust_pwm") == EXHAUST_IDLE && get_long("thermal/intake_pwm") == INTAKE_IDLE,
          "the loop recovers: the idle duties");
    L->bulk_c = 0.0;
    kv[0] = NULL;
    close_session(L);
    conf_reload();

    printf("gates A3. a diagnostic taken inside the tick that raises OVERTEMP: its release puts the cooling "
           "airflow on, and the TEC never runs with the exhaust at 0\n");
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    CHECK(!over_temp_gate && !strcmp(pub_phase, "idle"), "idle, the ceiling over the loop");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16";
    kv[4] = "cool_tec_present"; kv[5] = "1"; kv[6] = "cool_tec_on_c"; kv[7] = "15";
    kv[8] = "cool_tec_off_c"; kv[9] = "13"; kv[10] = NULL;
    conf_reload();
    take_on_overtemp = 1;
    took_in_tick = -1;
    fflog_hook = take_on_rise;
    run_idle_tick(L);
    fflog_hook = NULL;
    CHECK(took_in_tick == 1, "the take lands between the gate's rise and the tick's publish, and is granted");
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    cool_diag_release();
    tec_without_fans = 0;
    for (int i = 0; i < 40; i++) {
        run_idle_tick(L);
        if (get_long("thermal/tec_on") == 1 && get_long("thermal/exhaust_pwm") == 0)
            tec_without_fans = 1;
    }
    CHECK(!strcmp(pub_verdict, "OVERTEMP") && get_long("thermal/exhaust_pwm") == EXHAUST_COOL &&
          get_long("thermal/intake_pwm") == INTAKE_COOL, "after the release: OVERTEMP's cooling airflow");
    CHECK(!tec_without_fans, "the TEC never on with the exhaust at 0");
    kv[0] = NULL;
    L->bulk_c = -10.0;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    L->bulk_c = 0.0;
    conf_reload();
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

    /* Its cause gone under a job still held: the job's press is the operator's.
     * SENSOR goes once the readings have held long enough (SENSOR_FAIL_CAP). */
    L->dead_down = 0;
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
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
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
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
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
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
    for (int i = 0; i < SENSOR_FAIL_CAP; i++) {
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
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);               /* back at the tree's baseline */
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
    logbuf[0] = '\0';
    settle_and_open(L);
    CHECK(cool_state == Cool_Warmup && !strcmp(pub_verdict, "WARMUP") && pub_hold && !pub_fire_ok &&
          !strcmp(pub_phase, "warm-up"), "WARMUP, hold, fire blocked, phase warm-up");
    CHECK(get_long("thermal/heater_pwm") > 0, "the loop heater on");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_IDLE && get_long("thermal/intake_pwm") == INTAKE_IDLE &&
          get_long("head/air_assist_pwm") == AIR_ASSIST_IDLE, "the fans at their idle duties");
    /* A session that ends in its warm-up: the engine takes its heater off
     * (nothing else here writes it). */
    session_end_tick(L);
    CHECK(get_long("thermal/heater_pwm") == 0 && heater_pct == 0,
          "a session that ends in its warm-up: the engine's heater off");
    close_session(L);
    settle_and_open(L);
    CHECK(cool_state == Cool_Warmup, "the next session holds its warm-up again");
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
    CHECK(!log_has("fans at their floors in the armed window"),
          "no fan is judged in a warm-up, and nothing says they are up");
    /* The exhaust has not spun up when the release writes its run duty:
     * the release tick permits fire and must not acknowledge the window,
     * which waits for the run's fans to read their floors. */
    put_long("thermal/tach_exhaust", 10000000);     /* 3000 rpm: under the 6400 floor */
    logbuf[0] = '\0';
    run_tick(L);
    CHECK(cool_state == Cool_Run && get_long("thermal/heater_pwm") == 0,
          "the minute's minimum at the gate: released into run, the heater 0");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN && get_long("thermal/intake_pwm") == INTAKE_RUN,
          "the run duties");
    CHECK(log_has("warm-up complete"), "the line says the warm-up is complete");
    CHECK(pub_fire_ok && !pub_hold && !pub_armed_ack,
          "the release tick: fire permitted, the window not acknowledged");
    run_tick(L);
    run_tick(L);
    CHECK(!pub_armed_ack, "the exhaust still under its floor: no acknowledgment");
    put_long("thermal/tach_exhaust", 300);
    run_tick(L);
    CHECK(pub_armed_ack, "the exhaust at its floor: acknowledged");
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
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
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
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
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
    int rc = cool_quiet_hold_try(1, 1);
    CHECK(rc == 0 && flow_yields == y + 1 && flow_yield_quiet && flow_yield_pump == 1,
          "the route's pump hold: one yield in its own call, the hold already published, the pump not yet "
          "stopped");
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0, "the engine's next tick stops the pump");
    cool_quiet_hold(0);
    run_idle_tick(L);
    y = flow_yields;
    CHECK(cool_diag_take() == 0 && flow_yields == y + 1 && flow_yield_diag,
          "a diagnostic's take: one yield, the take already published");
    CHECK(cool_quiet_hold_try(1, 1) == -2 && !quiet_held,
          "the route's pump hold refused from it, before its first tick");
    CHECK(!strcmp(pub_phase, "idle") && cool_quiet_hold_try(1, 0) == -2 && cool_quiet_hold(1) == -2 && !quiet_held,
          "a fans-only hold and the finder's refused from it too, the phase still idle");
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

    /* A watch that cannot open or arm at its window is off for that
     * session alone: the next session's window claims the handle again,
     * opening it when it is gone, and arms. */
    close_session(L);
    crash_open_fail = 1;
    int opens = crash_open_calls;
    open_session(L);
    for (int i = 0; i < 3; i++)
        run_tick(L);
    CHECK(!crash_armed_fake && !strcmp(pub_accel_watch, "off") && crash_open_calls == opens + 1,
          "the handle is gone and does not open at the window: the watch is off for the session, "
          "tried once");
    crash_open_fail = 0;
    crash_arm_fail = 1;
    open_session(L);
    CHECK(!crash_armed_fake && !strcmp(pub_accel_watch, "off"),
          "the next session's window opens it and the arm fails: off for that session");
    crash_arm_fail = 0;
    open_session(L);
    CHECK(crash_armed_fake && !strcmp(pub_accel_watch, "armed"),
          "and the session after that arms the watch");

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
    printf("gates E. a setting or a GFCOOL_ variable that is not a value it takes: the default stands\n");
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
    /* A value is read whole, by the parse the settings route validates
     * with: padded past 16 characters (it would read as 0, the tier off),
     * longer than the read, out of the legal range, or a correction past
     * 5 C, the default stands. */
    kv[0] = "cool_fire_q1_critical"; kv[1] = "000000000000000000000";
    kv[2] = "cool_fire_q2_critical"; kv[3] = "00000000000000000000000000000000000000001022";
    kv[4] = "cool_temp_min"; kv[5] = "-5";
    kv[6] = "cool_temp_offset_c"; kv[7] = "-5e29";
    kv[8] = NULL;
    conf_reload();
    CHECK(fire_q1_critical == 688.0f && fire_q2_critical == 1022.0f && temp_min_c == TEMP_MIN_C_DEFAULT &&
          temp_offset_c == 0.0f && !strstr(pub_gates_off, "\"flame_q1_critical\"") &&
          !strstr(pub_gates_off, "\"coolant_min\""),
          "padded, too long, out of range, a correction past 5 C: each default stands, no gate off");
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
    session_end_tick(L);
    CHECK(cool_state != Cool_Idle && get_long("thermal/tec_on") == 1, "the cooldown keeps it on");
    CHECK(cool_quiet_hold_try(1, 0) == -2 && !quiet_held, "the route refuses a fans-only hold in the cooldown");
    cool_quiet_hold_ex(1, 0);           /* set past the route's check */
    loop_tick(L);
    tick("idle", 0);
    CHECK(get_long("thermal/exhaust_pwm") == 0 && get_long("thermal/tec_on") == 0,
          "a fans-only hold set past it in the cooldown: every fan off, and the TEC with them");
    cool_quiet_hold(0);
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

/* The pump and the TEC are written every tick: a TEC off-write the kernel
 * refuses is written again at the next tick, and a pump found off is put
 * back on. */
static void outputs_every_tick(loop_t *L)
{
    printf("TEC2. the pump and the TEC are written every tick: a refused write and a pump left off are put right\n");
    close_session(L);
    kv[0] = "cool_tec_present"; kv[1] = "1"; kv[2] = "cool_tec_on_c"; kv[3] = "15";
    kv[4] = "cool_tec_off_c"; kv[5] = "13"; kv[6] = NULL;
    settle_and_open(L);
    for (int i = 0; i < 120 && get_long("thermal/tec_on") != 1; i++)
        run_tick(L);
    CHECK(get_long("thermal/tec_on") == 1, "fitted, the loop over its threshold: the TEC on in the run");
    L->bulk_c = -10.0;                  /* 12.3 C: under cool_tec_off_c */
    drop_attr = "thermal/tec_on";
    drop_left = 1;
    refuse = 1;
    run_tick(L);
    CHECK(tec_on_state == 0 && get_long("thermal/tec_on") == 1, "the engine turns it off and the kernel refuses the write");
    drop_attr = NULL;
    refuse = 0;
    run_tick(L);
    CHECK(get_long("thermal/tec_on") == 0, "the next tick writes it again, and it takes");
    put_long("thermal/water_pump_on", 0);
    run_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 1, "a pump found off is put back on at the next tick");
    L->bulk_c = 0.0;
    kv[0] = NULL;
    close_session(L);

    printf("TEC3. a write that keeps failing is named once, until one takes; the next failure is named again\n");
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    int e0 = log_errs;
    drop_attr = "thermal/water_pump_on";
    drop_left = -1;
    refuse = 1;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    CHECK(log_errs - e0 == 1, "five refused pump writes: one line");
    drop_attr = NULL;
    drop_left = 0;
    refuse = 0;
    run_idle_tick(L);
    drop_attr = "thermal/water_pump_on";
    drop_left = -1;
    refuse = 1;
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    CHECK(log_errs - e0 == 2, "one took between them: the next refusal is named again, once");
    drop_attr = NULL;
    drop_left = 0;
    refuse = 0;
    run_idle_tick(L);
}

/* A quiet hold's posture is the engine's to write (quiet_apply): the hold
 * taken again without the pump gives the pump back, every release leaves it
 * on, and a session's opening tick that lands inside the hold's call leaves
 * no posture of the hold behind (the forced schedule: the tick sits where
 * the hold's yield falls). */
static void yield_tick(void);
static loop_t *yield_L;
static void yield_tick(void)
{
    yield_L->session = 1;
    loop_tick(yield_L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
}

/* A pump hold the route sets inside the tick that opens a run session: its
 * check reads the published phase, still idle for the whole tick, and its
 * call lands after the engine's look at the hold for the session's edge,
 * here where a fans-only hold that stood is released and the release is
 * logged. With @drop_inside the route takes the hold back again inside the
 * engine's own application of it. */
static int late_arm, late_drop_arm, late_ok = -1;
static void late_hold_hook(const char *line)
{
    if (late_arm && strstr(line, "quiet hold released: a run session opened")) {
        late_arm = 0;
        late_ok = cool_quiet_hold_try(1, 1) == 0;  /* the route's call */
    }
    if (late_drop_arm && strstr(line, "the pump and the TEC off too")) {
        late_drop_arm = 0;
        cool_quiet_hold(0);
    }
}

static void late_hold_open(loop_t *L, int into_warmup, int drop_inside)
{
    printf("quiet %s. a pump hold the route sets inside the session's opening tick, %s%s: the tick ends with "
           "the pump on, no heater on a stopped pump, no fire permitted on one\n",
           drop_inside ? "G" : into_warmup ? "F" : "E", into_warmup ? "into its warm-up" : "into its run",
           drop_inside ? ", taken back again the moment the engine applies it" : "");
    close_session(L);
    kv[0] = into_warmup ? "cool_temp_start" : NULL;
    kv[1] = "23.3";                     /* 1 C over the loop's 22.3 */
    kv[2] = NULL;
    conf_reload();
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    cool_quiet_hold(1);                 /* a fans-only hold stands */
    run_idle_tick(L);
    late_arm = 1;
    late_drop_arm = drop_inside;
    late_ok = -1;
    heat_with_pump_off = 0;
    fflog_hook = late_hold_hook;
    L->session = 1;
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);     /* opens armed */
    engine_tick();
    fflog_hook = NULL;
    long pump = get_long("thermal/water_pump_on");
    CHECK(late_ok == 1 && cool_state == (into_warmup ? Cool_Warmup : Cool_Run),
          "the route's check passed inside the opening tick, and the session opened");
    CHECK(pump == 1 && !quiet_pump_applied && !(pump == 0 && pub_fire_ok),
          "the opening tick ends with the pump on and the pump hold not applied");
    CHECK(drop_inside || !quiet_held, "a hold set inside the opening tick is released with the session's edge");
    CHECK(get_long("thermal/exhaust_pwm") == (into_warmup ? EXHAUST_IDLE : EXHAUST_RUN) &&
          get_long("head/air_assist_pwm") == (into_warmup ? AIR_ASSIST_IDLE : COOL_AIR_ASSIST_RUN),
          "and its fans with it, in that tick: the session's duties, none held off");
    for (int i = 0; i < 5; i++)
        run_tick(L);
    CHECK(heat_with_pump_off == 0 && get_long("thermal/water_pump_on") == 1,
          "five ticks on: no heater write landed with the pump off, the pump on");
    cool_quiet_hold(0);
    kv[0] = NULL;
    conf_reload();
    close_session(L);
}

/* A hold the route asks for in the tick that opens a session, after the
 * session's edge was published: in the session's own settings reload
 * (inside flood_apply, before the late look), and again at the run start's
 * limits line (past the late look). Refused both times, so the session
 * runs with its fans and, into a warm-up, its heater. */
static int refuse_arm, refuse_rc = 99, refuse_fans_rc = 99, refuse_late_arm, refuse_late_rc = 99,
           refuse_late_fans_rc = 99;
static void refuse_hook(const char *line)
{
    if (refuse_arm && strstr(line, "settings in force") && flood_on) {
        refuse_arm = 0;
        refuse_rc = cool_quiet_hold_try(1, 1);
        refuse_fans_rc = cool_quiet_hold_try(1, 0);
    }
    if (refuse_late_arm && strstr(line, "effective limits") && flood_on) {
        refuse_late_arm = 0;
        refuse_late_rc = cool_quiet_hold_try(1, 1);
        refuse_late_fans_rc = cool_quiet_hold_try(1, 0);
    }
}

static void refused_hold_open(loop_t *L, int into_warmup)
{
    printf("quiet %s. a pump hold the route asks for in the session's opening tick after its edge, %s: refused\n",
           into_warmup ? "L" : "K", into_warmup ? "into its warm-up" : "into its run");
    close_session(L);
    kv[0] = NULL;
    conf_reload();
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    /* A changed ceiling the session's own reload takes, so the run start
     * names its limits inside the opening tick. */
    int k = 0;
    kv[k++] = "cool_temp_max"; kv[k++] = "35"; kv[k++] = "cool_temp_resume"; kv[k++] = "33";
    if (into_warmup) {
        kv[k++] = "cool_temp_start"; kv[k++] = "23.3";     /* 1 C over the loop's 22.3 */
    }
    kv[k] = NULL;
    refuse_arm = refuse_late_arm = 1;
    refuse_rc = refuse_late_rc = refuse_fans_rc = refuse_late_fans_rc = 99;
    fflog_hook = refuse_hook;
    L->session = 1;
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);     /* opens armed */
    engine_tick();
    fflog_hook = NULL;
    CHECK(refuse_rc == -1 && refuse_late_rc == -1 && refuse_fans_rc == -1 && refuse_late_fans_rc == -1 &&
          !quiet_held && !quiet_pump_held,
          "the route's holds, with the pump and fans only, are refused before the late look and after it: "
          "no hold stands");
    int fans_off = 0, heat = 0, pump_off = 0;
    for (int i = 0; i < 8; i++) {
        run_tick(L);
        fans_off += get_long("thermal/exhaust_pwm") == 0 && get_long("head/air_assist_pwm") == 0 &&
                    get_long("thermal/intake_pwm") == 0;
        heat += get_long("thermal/heater_pwm") > 0;
        pump_off += get_long("thermal/water_pump_on") == 0;
    }
    if (into_warmup)
        CHECK(pump_off == 0 && heat > 0, "eight warm-up ticks: the pump on, the heater runs");
    else
        CHECK(pump_off == 0 && fans_off == 0, "eight run ticks: the pump on, the fans at their duties");
    kv[0] = NULL;
    conf_reload();
    close_session(L);
}

/* A hold the route took at idle, standing when the coolant crosses its
 * ceiling 30 s later, a TEC fitted: the hold is released in the tick the
 * gate rises, and the cooling airflow and the TEC run as they do with no
 * hold (@with_hold 0, the control). */
/* In the rising tick, right after the release puts the fans back: the
 * route's request (refused, the cooling airflow already published), then a
 * pump hold set past the route, which the tick's settle must not apply
 * under the gate. */
static int late_over_arm, late_over_rc = 99;
static void late_over_hook(const char *line)
{
    if (late_over_arm && over_temp_gate && strstr(line, "the fans back in their posture after the listening")) {
        late_over_arm = 0;
        late_over_rc = cool_quiet_hold_try(1, 1);
        cool_quiet_hold_ex(1, 1);
    }
}

static double over_airflow_s = -1, over_tec_s = -1;
static void quiet_over_ceiling(loop_t *L, int with_hold, int with_pump)
{
    printf("quiet P%s. the ceiling crossed at idle 30 s after %s: the cooling airflow in the rising tick\n",
           with_hold ? (with_pump ? "" : "f") : "c",
           with_hold ? (with_pump ? "a pump hold" : "a fans-only hold") : "no hold (the control)");
    close_session(L);
    kv[0] = "cool_tec_present"; kv[1] = "1"; kv[2] = "cool_tec_on_c"; kv[3] = "15";
    kv[4] = "cool_tec_off_c"; kv[5] = "13"; kv[6] = NULL;
    conf_reload();
    L->bulk_c = 0.0;
    for (int i = 0; i < 40; i++)
        run_idle_tick(L);
    if (with_hold)
        CHECK(cool_quiet_hold_try(1, with_pump) == 0, "the route takes the hold at idle");
    for (int i = 0; i < 30; i++)
        run_idle_tick(L);
    if (with_hold)
        CHECK(quiet_applied && get_long("head/air_assist_pwm") == 0 &&
              get_long("thermal/water_pump_on") == (with_pump ? 0 : 1),
              "the hold applied at idle: every fan off, the pump off with it or on without it");
    L->bulk_c = 13.0;                   /* 35.3 C: over the 33 C ceiling */
    double rise = -1, airflow = -1, tec = -1;
    int heat = 0, pump_off = 0;
    for (int i = 0; i < 700 && (airflow < 0 || tec < 0); i++) {
        run_idle_tick(L);
        if (rise < 0 && !strcmp(pub_verdict, "OVERTEMP"))
            rise = fake_now;
        if (rise < 0)
            continue;
        if (airflow < 0 && get_long("thermal/exhaust_pwm") == EXHAUST_COOL &&
            get_long("thermal/intake_pwm") == INTAKE_COOL)
            airflow = fake_now - rise;
        if (tec < 0 && get_long("thermal/tec_on") == 1)
            tec = fake_now - rise;
        heat += get_long("thermal/heater_pwm") > 0;
        pump_off += get_long("thermal/water_pump_on") == 0;
    }
    printf("    the cooling airflow %.0f s and the TEC %.0f s after the rise; hold standing %d\n", airflow, tec,
           quiet_held);
    CHECK(rise > 0 && airflow == 0 && !quiet_held && !quiet_pump_held && heat == 0 && pump_off == 0 &&
          !pub_fire_ok,
          "the rising tick: no hold, the cooling airflow, the pump on, no heater, fire blocked");
    if (with_hold)
        CHECK(airflow == over_airflow_s && tec == over_tec_s, "the cooling airflow and the TEC as with no hold");
    else {
        over_airflow_s = airflow;
        over_tec_s = tec;
    }
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    {
        char reason[COOL_REASON_MAX];
        pthread_mutex_lock(&mu);
        snprintf(reason, sizeof(reason), "%s", pub_reason);
        pthread_mutex_unlock(&mu);
        CHECK(!strcmp(pub_verdict, "OVERTEMP") && strstr(reason, "limit") && strstr(reason, "hold until"),
              "five ticks into the gate the reason is OVERTEMP's own: the coolant, its limit, the resume point");
    }
    L->bulk_c = 0.0;
    kv[0] = NULL;
    conf_reload();
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    close_session(L);
    (void)cool_attention_ack();
}

/* A fans-only hold the route takes in the cooldown after an armed session,
 * then the coolant crosses its ceiling in that cooldown: released in the
 * rising tick there too, the fans on over the gate. */
static void quiet_over_ceiling_cooldown(loop_t *L)
{
    printf("quiet Ps. a fans-only hold taken in a cooldown, then the ceiling crossed in it\n");
    close_session(L);
    kv[0] = "cool_cooldown_max_s"; kv[1] = "600"; kv[2] = NULL;
    L->bulk_c = 0.0;
    settle_and_open(L);
    for (int i = 0; i < 3; i++)
        run_tick(L);
    session_end_tick(L);
    int rc = cool_quiet_hold_try(1, 0);
    cool_quiet_hold_ex(1, 0);           /* set past the route's check */
    loop_tick(L);
    tick("idle", 0);
    CHECK(rc == -2 && cool_state != Cool_Idle && quiet_applied && get_long("thermal/exhaust_pwm") == 0,
          "the route refuses the hold in the cooldown; one set past it applied: every fan off");
    L->bulk_c = 13.0;                   /* 35.3 C: over the 33 C ceiling */
    int rose = 0, fans_off_over = 0;
    for (int i = 0; i < 8; i++) {
        loop_tick(L);
        tick("idle", 0);
        rose |= over_temp_gate;
        fans_off_over += over_temp_gate && get_long("thermal/exhaust_pwm") == 0;
    }
    CHECK(rose && cool_state != Cool_Idle && !quiet_held && fans_off_over == 0,
          "over the ceiling in the cooldown: the hold released in the rising tick, the fans on");
    L->bulk_c = -10.0;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    L->bulk_c = 0.0;
    kv[0] = NULL;
    conf_reload();
    close_session(L);
    (void)cool_attention_ack();
}

static void quiet_posture(loop_t *L)
{
    printf("quiet A. a quiet hold taken again without the pump gives the pump back; the release leaves it on\n");
    close_session(L);
    run_idle_tick(L);
    cool_quiet_hold_ex(1, 1);
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0 && get_long("thermal/tec_on") == 0,
          "a hold with the pump: the pump and the TEC off at the next tick");
    cool_quiet_hold_ex(1, 0);
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 1, "taken again without the pump: the pump back on");
    cool_quiet_hold_ex(0, 0);
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 1 && !quiet_held, "released: the pump on");

    printf("quiet B. a session's opening tick inside the hold's call: the session runs with the pump on and its run duties\n");
    close_session(L);
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    yield_L = L;
    yield_ticks_session = 1;
    cool_quiet_hold_ex(1, 1);
    for (int i = 0; i < 10; i++)
        run_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 1 && get_long("thermal/exhaust_pwm") == EXHAUST_RUN &&
          get_long("thermal/intake_pwm") == INTAKE_RUN && !quiet_held,
          "ten ticks on: the pump on, the run duties, no hold standing");
    close_session(L);

    printf("quiet C. a pump hold the route took at idle, set after a session opened armed: the session runs "
           "with the pump on, the hold waiting\n");
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    pthread_mutex_lock(&mu);
    int ok = quiet_pump_ok_locked();
    pthread_mutex_unlock(&mu);
    L->session = 1;
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
    cool_quiet_hold_ex(1, 1);
    int pump_off_in_run = 0;
    for (int i = 0; i < 30; i++) {
        run_tick(L);
        if (get_long("thermal/water_pump_on") == 0)
            pump_off_in_run = 1;
    }
    CHECK(ok && !pump_off_in_run && !quiet_pump_applied && !strcmp(pub_phase, "run"),
          "thirty run ticks: the pump on throughout, the pump hold not applied");
    cool_quiet_hold(0);
    close_session(L);

    printf("quiet D. a pump hold at idle, then a session that opens into its warm-up: the pump is back on "
           "before the warm-up heater is written\n");
    run_idle_tick(L);
    cool_quiet_hold_ex(1, 1);
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0, "the hold at idle: the pump off");
    kv[0] = "cool_temp_start"; kv[1] = "23.3"; kv[2] = NULL;     /* 1 C over the loop's 22.3 */
    heat_with_pump_off = 0;
    heater_written_max = 0;
    settle_and_open(L);
    CHECK(cool_state == Cool_Warmup && heater_written_max > 0 && heat_with_pump_off == 0 &&
          get_long("thermal/water_pump_on") == 1 && !quiet_held,
          "the session's opening tick: the hold released, the pump on, then the heater");
    kv[0] = NULL;
    close_session(L);

    late_hold_open(L, 0, 0);
    late_hold_open(L, 1, 0);
    late_hold_open(L, 1, 1);
    refused_hold_open(L, 0);
    refused_hold_open(L, 1);

    printf("quiet N. a fans-only hold the route asks for five ticks into an armed run: refused; a release "
           "is taken\n");
    close_session(L);
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    for (int i = 0; i < 5; i++)
        run_tick(L);
    {
        int rc = cool_quiet_hold_try(1, 0), fans_off_fire = 0;
        for (int i = 0; i < 8; i++) {
            run_tick(L);
            fans_off_fire += get_long("thermal/exhaust_pwm") == 0 && pub_fire_ok;
        }
        CHECK(rc == -1 && !quiet_held && fans_off_fire == 0, "refused: the run keeps its fans");
        CHECK(cool_quiet_hold_try(0, 0) == 0 && !quiet_held, "a release is taken in the session");
    }
    close_session(L);

    printf("quiet O. the route's pump hold in the smoke clear after an armed session: refused (-2)\n");
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    for (int i = 0; i < 3; i++)
        run_tick(L);
    L->session = 0;
    run_idle_tick(L);                   /* the session's close: the smoke clear */
    {
        int rc = cool_quiet_hold_try(1, 1);
        CHECK(strcmp(pub_phase, "idle") && !pub_session && rc == -2 && !quiet_held,
              "the cooldown: no session, the pump hold refused as a busy engine");
    }
    /* No hold stands over an airflow the engine runs for safety: the
     * route's fans-only hold and the finder's are refused in the smoke
     * clear and in the thermal cooldown, and the fans keep their duties. */
    {
        int rc_fans = cool_quiet_hold_try(1, 0), rc_finder = cool_quiet_hold(1), fans_off = 0;
        run_idle_tick(L);
        fans_off += get_long("thermal/exhaust_pwm") != EXHAUST_RUN;
        CHECK(cool_state == Cool_Smoke && rc_fans == -2 && rc_finder == -2 && !quiet_held && !fans_off,
              "the smoke clear: a fans-only hold and the finder's refused, the run airflow stands");
    }
    close_session(L);
    {
        double bulk_was = L->bulk_c;
        L->bulk_c = 10.0;               /* about 32 C: over the 31 C resume gate, under the 33 C ceiling */
        settle_and_open(L);
        for (int i = 0; i < 3; i++)
            run_tick(L);
        L->session = 0;
        int thermal = 0;
        for (int i = 0; i < 40 && !thermal; i++) {
            run_idle_tick(L);
            thermal = cool_state == Cool_Thermal;
        }
        int rc_fans = cool_quiet_hold_try(1, 0), rc_finder = cool_quiet_hold(1);
        run_idle_tick(L);
        CHECK(thermal && cool_state == Cool_Thermal && rc_fans == -2 && rc_finder == -2 && !quiet_held &&
              get_long("thermal/exhaust_pwm") == EXHAUST_COOL,
              "the thermal cooldown: a fans-only hold and the finder's refused, the cooling airflow stands");
        L->bulk_c = bulk_was;
    }
    close_session(L);

    quiet_over_ceiling(L, 0, 0);
    quiet_over_ceiling(L, 1, 1);
    quiet_over_ceiling(L, 1, 0);
    quiet_over_ceiling_cooldown(L);

    printf("quiet Q. a hold asked for right after the release over the ceiling, in the rising tick: the route "
           "refused, and one set past it not applied\n");
    close_session(L);
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    CHECK(cool_quiet_hold_try(1, 1) == 0, "the route takes a pump hold at idle");
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0, "applied: the pump off");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    conf_reload();
    late_over_arm = 1;
    late_over_rc = 99;
    fflog_hook = late_over_hook;
    run_idle_tick(L);
    fflog_hook = NULL;
    CHECK(over_temp_gate && late_over_rc == -3, "the rising tick: the route's request after the release refused");
    CHECK(get_long("thermal/water_pump_on") == 1 && !quiet_pump_applied,
          "and a pump hold set past the route there is not applied under the gate: the pump on");
    run_idle_tick(L);
    CHECK(!quiet_held && get_long("thermal/exhaust_pwm") == EXHAUST_COOL, "the next tick releases it, the cooling airflow");
    kv[0] = NULL;
    L->bulk_c = -10.0;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    L->bulk_c = 0.0;
    conf_reload();
    close_session(L);
    (void)cool_attention_ack();

    printf("quiet M. a pump hold that runs past its bound: released, the pump back on\n");
    close_session(L);
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    CHECK(cool_quiet_hold_try(1, 1) == 0, "the route takes a pump hold at idle");
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0, "the hold at idle: the pump off");
    pthread_mutex_lock(&mu);
    quiet_since = fake_now - (COOL_QUIET_HOLD_MAX_S + 1);   /* a listener that died */
    pthread_mutex_unlock(&mu);
    run_idle_tick(L);
    CHECK(!quiet_held && !quiet_pump_held && get_long("thermal/water_pump_on") == 1,
          "past the bound: the hold released, the pump on");

    printf("quiet H. a pump hold applied at idle, then the tick that raises OVERTEMP: the pump back on in that tick\n");
    close_session(L);
    for (int i = 0; i < 3; i++)
        run_idle_tick(L);
    cool_quiet_hold_ex(1, 1);
    run_idle_tick(L);
    CHECK(get_long("thermal/water_pump_on") == 0 && quiet_pump_applied, "the hold at idle: the pump off");
    kv[0] = "cool_temp_max"; kv[1] = "18"; kv[2] = "cool_temp_resume"; kv[3] = "16"; kv[4] = NULL;
    conf_reload();
    run_idle_tick(L);
    CHECK(over_temp_gate && get_long("thermal/water_pump_on") == 1 && !quiet_pump_applied && !quiet_held,
          "the tick that raises the gate releases the whole hold and ends with the pump on");
    kv[0] = NULL;
    L->bulk_c = -10.0;
    for (int i = 0; i < 5; i++)
        run_idle_tick(L);
    L->bulk_c = 0.0;
    conf_reload();
    close_session(L);
    (void)cool_attention_ack();

    printf("quiet J. a pump hold that waits through a session: applied in no warm-up, smoke clear or cooldown, "
           "the warm-up's heater held off meanwhile, then applied by itself at the idle after them\n");
    close_session(L);
    kv[0] = "cool_temp_start"; kv[1] = "23.3"; kv[2] = "cool_temp_max"; kv[3] = "25";
    kv[4] = "cool_temp_resume"; kv[5] = "20"; kv[6] = "cool_cooldown_max_s"; kv[7] = "5"; kv[8] = NULL;
    conf_reload();
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
    cool_quiet_hold_ex(1, 1);           /* landed past the route's check */
    int warm_pump_off = 0, warm_heat = 0;
    for (int i = 0; i < 10; i++) {
        run_tick(L);
        warm_pump_off += get_long("thermal/water_pump_on") == 0;
        warm_heat += get_long("thermal/heater_pwm") > 0;
    }
    CHECK(cool_state == Cool_Warmup && warm_pump_off == 0 && warm_heat == 0,
          "ten warm-up ticks: the pump on, the hold not applied, the heater held off");
    L->session = 0;
    int seen_smoke = 0, seen_thermal = 0, off_outside_idle = 0, off_at_idle = 0;
    for (int i = 0; i < 200 && !off_at_idle; i++) {
        loop_tick(L);
        tick("idle", 0);
        seen_smoke |= cool_state == Cool_Smoke;
        seen_thermal |= cool_state == Cool_Thermal;
        if (get_long("thermal/water_pump_on") == 0) {
            if (cool_state == Cool_Idle)
                off_at_idle = 1;
            else
                off_outside_idle = 1;
        }
    }
    CHECK(seen_smoke && seen_thermal && !off_outside_idle,
          "the smoke clear and the cooldown: the pump on throughout");
    CHECK(off_at_idle && quiet_pump_applied, "at the idle after them the waiting hold applies by itself");
    cool_quiet_hold(0);
    run_idle_tick(L);
    kv[0] = NULL;
    conf_reload();
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
            for (int k = 0; k < SENSOR_FAIL_CAP; k++)
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
    for (int k = 0; k < SENSOR_FAIL_CAP; k++)
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
    lid_ir_all(100);                    /* back at the tree's baseline */
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
    run_idle_tick(L);
    CHECK(get_long("cnc/laser_latch_cooling") == 1,
          "emission with no armed window: the engine's latch hold stands while it is sensed");
    put_long("cnc/laser_on_sampled", 0);
    run_idle_tick(L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "and is released once the witness reads none");
    CHECK(events == ev + 1, "an emission with no armed window");
    put("cnc/stop", "0\n");
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
 * order shows. The TEC goes off before the engine's join (an engine that
 * hangs never returns from it) and again after it (a tick in flight wrote
 * it on). Last: the engine is not started again. */
static void shutdown_order(loop_t *L)
{
    printf("the shutdown: the flow watch stopped first, then the engine's heater off; the TEC off before the join and after\n");
    close_session(L);
    kv[0] = "cool_temp_start"; kv[1] = "30"; kv[2] = NULL;
    settle_and_open(L);
    long heat = get_long("thermal/heater_pwm");
    kv[0] = NULL;
    flow_stops = 0;
    put_long("thermal/tec_on", 1);      /* a TEC left on */
    put_long("thermal/water_pump_on", 0);   /* a pump hold's pump off */
    join_writes_tec = 1;
    joins = 0;
    engine_run = 1;                     /* the fake join stands in for the thread */
    cool_shutdown();
    CHECK(heat > 0 && flow_stops == 1 && flow_stop_heater == heat && get_long("thermal/heater_pwm") == 0,
          "the watch stopped once with the warm-up's heat still on, then the heater off");
    CHECK(joins == 1 && tec_at_join == 0 && get_long("thermal/tec_on") == 0,
          "the TEC off before the join, and off again after the tick the join waited for wrote it on");
    CHECK(pump_at_join == 1 && get_long("thermal/water_pump_on") == 1,
          "the pump on before the join, and on again after the tick the join waited for wrote it off");
}

/* A count that is no temperature (coolant.h) is no reading, as a read that
 * fails is: SENSOR at the second tick, the heater off. Judged on the count
 * as read: at the legal maximum air-assist share (60 counts at the run
 * duty) and correction (+5 C), 990 counts would convert to about +9 C. A
 * count of 0 in a run is a short, not a dropout (gates F). */
static void counts_no_reading(loop_t *L)
{
    static const long counts[] = { 1023, 65535, 990 };
    char msg[128];
    printf("H2. a count that is no temperature: SENSOR, judged before the corrections\n");
    kv[0] = "cool_aa_offset_counts"; kv[1] = "60"; kv[2] = "cool_temp_offset_c"; kv[3] = "5";
    kv[4] = NULL;
    open_session(L);
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK") && cool_coolant_offset_counts() == 60,
          "a settled session at the full air-assist share and +5 C: fire permitted");
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        L->fix_up = counts[i];
        run_tick(L);
        run_tick(L);
        snprintf(msg, sizeof(msg), "upstream at count %ld: SENSOR, fire blocked, hold, the count named",
                 counts[i]);
        CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR") && heater_pct == 0 &&
              strstr(pub_reason, "upstream"), msg);
        L->fix_up = -1;
        for (int k = 0; k < SENSOR_FAIL_CAP; k++)
            run_tick(L);
        CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "the reading back: released");
    }
    L->fix_down = 1023;
    run_tick(L);
    run_tick(L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR") && strstr(pub_reason, "downstream"),
          "downstream at the rail: SENSOR too");
    L->fix_down = -1;
    for (int k = 0; k < SENSOR_FAIL_CAP; k++)
        run_tick(L);
    CHECK(isnan(cool_coolant_c(1023)) && isnan(cool_coolant_c(0)) && !isnan(cool_coolant_c(700)),
          "a sample's degrees from a count that is no temperature: none (NaN)");
    kv[0] = NULL;
    close_session(L);
}

/* A coolant reading over 70 C (a short reads a few counts over 0, hundreds
 * of degrees) is no reading, whatever the gates are set to. In a run session
 * it is SENSOR's fail tier in its own tick: fire blocked, hold, cnc/stop and
 * the engine's latch hold written, all standing until the job has ended,
 * whatever the readings do meanwhile. With the floor, the ceiling, the
 * critical line and the warm-up gate all at their off ends no gate would
 * judge it. It is judged on the count's own conversion and after the
 * corrections. At idle it is the plain SENSOR: no pair, released with the
 * readings, no cooling airflow forced on it, and a TEC fitted never driven. */
static void pair_cleared(void)
{
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
}

static int short_failed(const char *which)
{
    return !pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR") && strstr(pub_reason, which) &&
           get_long("cnc/stop") == 1 && get_long("cnc/laser_latch_cooling") == 1;
}

static void gates_off_session(loop_t *L, const char *offset)
{
    close_session(L);
    kv[0] = "cool_temp_min"; kv[1] = "0"; kv[2] = "cool_temp_max"; kv[3] = "60";
    kv[4] = "cool_temp_critical_c"; kv[5] = "70"; kv[6] = "cool_temp_start"; kv[7] = "0";
    kv[8] = "cool_temp_offset_c"; kv[9] = offset; kv[10] = NULL;
    settle_and_open(L);
    run_tick(L);
}

static void near_short_reads(loop_t *L, const char *offset, double c, int whole, const char *msg)
{
    gates_off_session(L, offset);
    CHECK(cool_state == Cool_Run && pub_fire_ok && !strcmp(pub_verdict, "OK") &&
          strstr(pub_gates_off, "\"coolant_min\"") && strstr(pub_gates_off, "\"coolant_max\"") &&
          strstr(pub_gates_off, "\"coolant_critical\""),
          "the floor, the ceiling and the critical line at their off ends, a settled run: fire permitted");
    pair_cleared();
    stops = 0;
    L->fix_up = c > 0 ? raw_for(c) : 3;
    run_tick(L);
    CHECK(short_failed("upstream"), msg);
    L->fix_up = -1;
    if (whole) {
        int stood = 1;
        for (int k = 0; k < SENSOR_FAIL_CAP + 6; k++) {
            run_tick(L);
            stood &= !pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR") &&
                     get_long("cnc/laser_latch_cooling") == 1;
        }
        CHECK(stood, "the reading back: SENSOR, the hold and the engine's latch hold stand, no resume");
        kv[0] = NULL;
        close_session(L);
        CHECK(get_long("cnc/laser_latch_cooling") == 0 && !sensor_alarm && stops == 1,
              "the job ended (idle reported): the latch hold released, SENSOR gone with the readings, no "
              "second restart");
    }
    kv[0] = NULL;
    close_session(L);
}

/* The line's own edge, every gate off: count 245 (70.09 C) is a short; a
 * reading of exactly 70.0 C is a temperature. */
static void near_short_edge(loop_t *L)
{
    gates_off_session(L, "0");
    L->fix_up = 245;
    run_tick(L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR"),
          "upstream at count 245 (70.09 C), every gate off: SENSOR in its own tick");
    L->fix_up = -1;
    gates_off_session(L, "0");
    degc_exact_raw = 250;
    degc_exact_c = 70.0;
    L->fix_up = 250;
    run_tick(L);
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK") && !sensor_alarm,
          "upstream at exactly 70.0 C, every gate off: a temperature, fire permitted");
    degc_exact_raw = -1;
    L->fix_up = -1;
    kv[0] = NULL;
    close_session(L);
}

/* The shipped gates (gates.c): a near-short frame in a run is SENSOR's fail
 * tier in its own tick, on either sensor; one every 12 s, under the dropout
 * window's six in 60 s, ends the job at its first frame. */
static void shipped_run(loop_t *L)
{
    close_session(L);
    settle_and_open(L);
    double t0 = fake_now;
    while (fake_now < t0 + FAN_GRACE_S + 1)
        run_tick(L);
}

/* One tick with no report at all (a silent or stopped controller). */
static void tick_silent(loop_t *L)
{
    loop_tick(L);
    fake_now += 1.0;
    engine_tick();
}

static void near_short_shipped(loop_t *L)
{
    shipped_run(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "the shipped gates, a settled run past the grace: fire permitted");
    pair_cleared();
    stops = 0;
    L->fix_up = 3;
    run_tick(L);
    CHECK(short_failed("upstream"),
          "the shipped gates, upstream at count 3 (about 371 C): SENSOR's fail tier and the kernel pair in the "
          "frame's own tick");
    CHECK(stops == 1 && stop_seen_stop == 1 && stop_seen_latch == 1 && !strcmp(stop_why, "coolant sensor short"),
          "and the controller ended through the supervisor, once, after the kernel pair");
    /* The supervisor's stop of that controller (no report since): the job
     * is gone with it. */
    L->fix_up = -1;
    cool_controller_stopped();
    for (int k = 0; k < SENSOR_FAIL_CAP + 1; k++)
        tick_silent(L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0 && !sensor_alarm && !pub_fire_ok && stops == 1,
          "no controller remains: the tier released, fire still blocked, no second restart");
    close_session(L);

    shipped_run(L);
    pair_cleared();
    int first = -1, open_after = 0;
    for (int f = 0; f < 5; f++) {
        for (int k = 0; k < 11; k++) {
            run_tick(L);
            open_after |= first >= 0 && pub_fire_ok;
        }
        L->fix_up = 3;
        run_tick(L);
        if (first < 0 && short_failed("upstream"))
            first = f;
        open_after |= first >= 0 && pub_fire_ok;
        L->fix_up = -1;
    }
    CHECK(first == 0 && !open_after,
          "one near-short frame every 12 s: the job ended at the first frame, fire never permitted after it");
    close_session(L);

    shipped_run(L);
    pair_cleared();
    stops = 0;
    L->fix_down = 3;
    run_tick(L);
    CHECK(short_failed("downstream") && stops == 1,
          "downstream at count 3: SENSOR's fail tier, the kernel pair and the restart in its own tick, the "
          "downstream sensor named");
    L->fix_down = -1;
    close_session(L);
}

/* One tick with the controller's report as given. */
static void tick_report(loop_t *L, const char *mode, int armed)
{
    loop_tick(L);
    fake_now += 1.0;
    cool_state_report(mode, armed, -1, -1, -1, NULL);
    engine_tick();
}

static int short_stands(void)
{
    return !pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR") && get_long("cnc/laser_latch_cooling") == 1;
}

/* The short's words, and the kernel pair as it stood when they were logged. */
static long short_pair_at_words;
static void short_words_hook(const char *line)
{
    if (strstr(line, "a short") && strstr(line, "job stopped") && short_pair_at_words == -2)
        short_pair_at_words = get_long("cnc/stop") * 10 + get_long("cnc/laser_latch_cooling");
}

/* The short's fail tier, rule by rule: the pair before the words, stop
 * then latch, the pair and the controller's end once; until the
 * supervisor's stop of that controller lands (the stopped controller
 * releases it, near_short_shipped) it stands through a silence, an idle
 * report with a window open, a closed window in an open session
 * (an M8 left on), and past the session's end while the controller still
 * reports the job (a window that ran out of grace reports cooldown; an
 * Acknowledge and a re-arm meet it too); it ends when the controller
 * reports idle with no window. */
static void short_tier_rules(loop_t *L)
{
    shipped_run(L);
    pair_cleared();
    write_turn = stop_turn = cool_lock_turn = 0;
    short_pair_at_words = -2;
    fflog_hook = short_words_hook;
    L->fix_up = 3;
    run_tick(L);
    fflog_hook = NULL;
    CHECK(short_pair_at_words == 11, "a short: cnc/stop and the engine's latch hold written before the words");
    CHECK(stop_turn > 0 && cool_lock_turn > stop_turn, "a short: cnc/stop first, then the engine's latch hold");
    put("cnc/stop", "0\n");
    stops = 0;
    run_tick(L);
    run_tick(L);
    CHECK(get_long("cnc/stop") == 0 && stops == 0, "a short that stands: the stop and the controller's end once");
    L->fix_up = -1;
    /* The controller's end is the supervisor's to carry out; until it
     * reports or stops, the tier stands on whatever it reports. */
    int stood = 1;
    for (int k = 0; k < 10; k++) {
        tick_silent(L);
        stood &= get_long("cnc/laser_latch_cooling") == 1 && short_alarm;
    }
    tick_unarmed_run(L);
    stood &= short_stands();
    CHECK(stood, "a silent controller, then a report again: the tier stands");
    for (int k = 0; k < 3; k++) {
        tick_unarmed_run(L);
        stood &= short_stands();
    }
    CHECK(stood, "the window closed with M8 on: SENSOR, the hold and the engine's latch hold stand");
    tick_report(L, "idle", 1);
    CHECK(short_stands(), "idle reported with a window open: the tier stands");
    stood = 1;
    for (int k = 0; k < 70; k++) {
        tick_report(L, "cooldown", 0);
        stood &= short_stands();
    }
    CHECK(stood && cool_state != Cool_Run,
          "the window ran out with no M8 (cooldown reported, the session ended): the tier stands, no ACK, no fire");
    (void)cool_attention_ack();
    tick_report(L, "cooldown", 0);
    stood = short_stands();
    for (int k = 0; k < 3; k++) {
        tick_report(L, "run", 1);
        stood &= short_stands();
    }
    CHECK(stood, "an Acknowledge and a re-arm while the controller still reports the job: refused, the tier stands");
    tick_report(L, "idle", 0);
    tick_report(L, "idle", 0);
    CHECK(get_long("cnc/laser_latch_cooling") == 0 && !sensor_alarm && !strcmp(pub_verdict, "ACK") && !pub_fire_ok,
          "the controller reports idle, no window: the job has ended, the tier released, ACK");
    close_session(L);

    /* A session no window ever opened in (M8 alone). */
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    {
        double t0 = fake_now;
        while (fake_now < t0 + FAN_GRACE_S + 1)
            tick_unarmed_run(L);
    }
    pair_cleared();
    stops = 0;
    L->fix_up = 3;
    tick_unarmed_run(L);
    CHECK(short_failed("upstream") && stops == 1,
          "an unarmed session: a short takes SENSOR's fail tier, the kernel pair and the restart");
    L->fix_up = -1;
    close_session(L);

    /* Over a SENSOR already standing (an open sensor, then a short). */
    shipped_run(L);
    pair_cleared();
    stops = 0;
    L->fix_up = 1023;
    run_tick(L);
    run_tick(L);
    int plain = !strcmp(pub_verdict, "SENSOR") && get_long("cnc/stop") == 0 && stops == 0;
    L->fix_up = 3;
    run_tick(L);
    int failed = short_failed("upstream") && stops == 1;
    L->fix_up = -1;
    stood = 1;
    for (int k = 0; k < SENSOR_FAIL_CAP + 2; k++) {
        run_tick(L);
        stood &= short_stands();
    }
    CHECK(plain && failed && stood,
          "an open sensor in a run: the plain SENSOR, no restart; a short over it: the fail tier, the pair and "
          "the restart, no resume");
    close_session(L);

    /* A stop the kernel refuses: the latch lock lands all the same. */
    shipped_run(L);
    pair_cleared();
    drop_attr = "cnc/stop";
    refuse = 1;
    drop_left = -1;
    L->fix_up = 3;
    run_tick(L);
    drop_attr = NULL;
    refuse = 0;
    drop_left = 0;
    CHECK(get_long("cnc/laser_latch_cooling") == 1 && !pub_fire_ok && !strcmp(pub_verdict, "SENSOR"),
          "a short with the stop refused: the engine's latch hold locked all the same");
    L->fix_up = -1;
    close_session(L);

    /* A short in a job's tail (the smoke clear) is the plain SENSOR. */
    shipped_run(L);
    run_idle_tick(L);
    pair_cleared();
    stops = 0;
    L->fix_up = 3;
    run_idle_tick(L);
    CHECK(cool_state == Cool_Smoke && !strcmp(pub_verdict, "SENSOR") && get_long("cnc/stop") == 0 &&
          get_long("cnc/laser_latch_cooling") == 0 && stops == 0,
          "a short in the smoke clear: the plain SENSOR, no kernel pair, no controller stop");
    L->fix_up = -1;
    close_session(L);
}

/* A count of 0 (the shorted thermistor itself, coolant.h) in a run is the
 * short's fail tier, on either sensor, in its own tick; one every 12 s ends
 * the job at its first frame. At idle it stays a dropout: tolerated for a
 * tick, SENSOR at the second, no pair. */
static void zero_count_in_run(loop_t *L)
{
    shipped_run(L);
    pair_cleared();
    stops = 0;
    L->fix_up = 0;
    run_tick(L);
    CHECK(short_failed("upstream") && strstr(pub_reason, "count 0") && stops == 1 &&
          !strcmp(stop_why, "coolant sensor short"),
          "the shipped gates, upstream at count 0: SENSOR's fail tier, the kernel pair and the restart in its own "
          "tick");
    /* The job ends at once (idle reported): SENSOR keeps a short's full
     * score, so it stands through two reading ticks and goes after four. */
    L->fix_up = -1;
    tick_report(L, "idle", 0);
    tick_report(L, "idle", 0);
    int still = sensor_alarm;
    tick_report(L, "idle", 0);
    tick_report(L, "idle", 0);
    CHECK(still && !sensor_alarm, "after a count-0 trip and the job's end: SENSOR goes after four reading ticks");
    close_session(L);

    shipped_run(L);
    pair_cleared();
    L->fix_down = L->fix_up = 0;
    run_tick(L);
    CHECK(short_failed("sensors"), "both sensors at count 0 in a run: SENSOR's fail tier and the kernel pair");
    L->fix_down = L->fix_up = -1;
    close_session(L);

    shipped_run(L);
    run_idle_tick(L);
    pair_cleared();
    L->fix_up = 0;
    run_idle_tick(L);
    CHECK(cool_state == Cool_Smoke && get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 0,
          "a count of 0 in the smoke clear: a dropout, no kernel pair");
    L->fix_up = -1;
    close_session(L);

    pair_cleared();
    L->fix_up = 0;
    settle_and_open(L);
    run_tick(L);
    CHECK(cool_state == Cool_Warmup && get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 0,
          "a count of 0 as a warm-up session opens: no kernel pair");
    L->fix_up = -1;
    close_session(L);

    shipped_run(L);
    pair_cleared();
    int first = -1, open_after = 0;
    for (int f = 0; f < 5; f++) {
        for (int k = 0; k < 11; k++) {
            run_tick(L);
            open_after |= first >= 0 && pub_fire_ok;
        }
        L->fix_up = 0;
        run_tick(L);
        if (first < 0 && short_failed("upstream"))
            first = f;
        open_after |= first >= 0 && pub_fire_ok;
        L->fix_up = -1;
    }
    CHECK(first == 0 && !open_after, "one count-0 frame every 12 s: the job ended at the first frame");
    close_session(L);

    shipped_run(L);
    pair_cleared();
    L->fix_down = 0;
    run_tick(L);
    CHECK(short_failed("downstream"), "downstream at count 0: SENSOR's fail tier and the kernel pair");
    L->fix_down = -1;
    close_session(L);

    close_session(L);
    pair_cleared();
    L->fix_up = 0;
    run_idle_tick(L);
    int tolerated = strcmp(pub_verdict, "SENSOR") != 0;
    run_idle_tick(L);
    CHECK(tolerated && !strcmp(pub_verdict, "SENSOR") && get_long("cnc/stop") == 0 &&
          get_long("cnc/laser_latch_cooling") == 0,
          "at idle, upstream at count 0: a dropout, tolerated a tick, SENSOR at the second, no pair");
    L->fix_up = -1;
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
        run_idle_tick(L);
}

static void cold_side(loop_t *L)
{
    printf("gates F. a near-short coolant sensor ends a run in its own tick, whatever the gates are set to; "
           "at idle it is SENSOR\n");
    void (*stop_was)(const char *) = cool_fail_tier_stop;
    cool_fail_tier_stop = fake_stop;
    near_short_reads(L, "0", -1.0, 1,
                     "upstream at count 3 (about 371 C), every gate off: SENSOR's fail tier and the kernel pair "
                     "in its own tick");
    near_short_reads(L, "-5", 72.0, 0,
                     "72 C on the count's own conversion, 67 C after a -5 C correction: SENSOR's fail tier");
    near_short_reads(L, "5", 67.0, 0,
                     "67 C on the count's own conversion, 72 C after a +5 C correction: SENSOR's fail tier");
    near_short_edge(L);
    near_short_shipped(L);
    short_tier_rules(L);
    zero_count_in_run(L);

    /* The warm-up gate on: the session opens undecided, no heat, and SENSOR
     * names it; the warm-up is no run, so no kernel pair. */
    pair_cleared();
    stops = 0;
    L->fix_up = 3;
    settle_and_open(L);
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && cool_state == Cool_Warmup && warmup_pending && heater_pct == 0 &&
          !pub_armed_ack && !strcmp(pub_verdict, "SENSOR") && get_long("cnc/stop") == 0 &&
          get_long("cnc/laser_latch_cooling") == 0 && stops == 0,
          "the warm-up gate on: undecided, no heat, fire blocked, the plain SENSOR, no kernel pair, no controller "
          "stop");
    L->fix_up = -1;
    close_session(L);

    /* At idle, the ceiling on and a TEC fitted. */
    kv[0] = "cool_tec_present"; kv[1] = "1"; kv[2] = NULL;
    close_session(L);
    pair_cleared();
    stops = 0;
    L->fix_up = 3;
    run_idle_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && !pub_fire_ok, "at idle, upstream at count 3: SENSOR in its own tick");
    int forced = 0, tec = 0;
    for (int i = 0; i < TEC_DWELL_S + 5; i++) {
        run_idle_tick(L);
        forced |= forced_cool || over_temp_gate || get_long("thermal/exhaust_pwm") != EXHAUST_IDLE;
        tec |= get_long("thermal/tec_on") != 0 || tec_on_state;
    }
    CHECK(cool_state == Cool_Idle && !strcmp(pub_verdict, "SENSOR") && !pub_fire_ok && !forced &&
          get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 0 && stops == 0,
          "at idle, the ceiling on: the plain SENSOR, no kernel pair, no controller stop, no cooling airflow "
          "forced on it");
    CHECK(!tec, "and a TEC fitted is never driven on it");
    L->fix_up = -1;
    for (int i = 0; i < SENSOR_FAIL_CAP; i++)
        run_idle_tick(L);
    CHECK(!sensor_alarm, "the reading back: SENSOR released");
    kv[0] = NULL;
    close_session(L);
    cool_fail_tier_stop = stop_was;
}

/* The hold's reason tells the truth about the warm-up: never "heater off"
 * with the heater on, and never empty while the verdict is WARMUP. */
static int warmup_reason_true(void)
{
    if (strstr(pub_reason, "heater off") && heater_pct > 0)
        return 0;
    return strcmp(pub_verdict, "WARMUP") != 0 || pub_reason[0] != '\0';
}

/* The warm-up is decided on a reading. A session that opens with the gate
 * on and no upstream reading holds undecided: WARMUP, the heater off, the
 * armed window not acknowledged (a controller's arm waits rather than read
 * the hold as a refusal). The first reading decides: under the gate, the
 * warm-up proper; at it or over, the run. */
static void warmup_undecided(loop_t *L)
{
    printf("gates B2. a session that opens with no upstream reading: undecided until one\n");
    for (int cold = 1; cold >= 0; cold--) {
        close_session(L);
        if (cold) {
            kv[0] = "cool_temp_start"; kv[1] = "23.3"; kv[2] = NULL;    /* over the loop's 22.3 */
        }
        for (int i = 0; i < 20; i++)
            run_idle_tick(L);
        L->session = 1;
        L->dead_up = 1;                 /* the opening tick's read fails */
        run_tick(L);
        CHECK(cool_state == Cool_Warmup && warmup_pending && !strcmp(pub_verdict, "WARMUP") &&
              !pub_fire_ok && pub_hold && heater_pct == 0 && !pub_armed_ack,
              cold ? "a cold loop: WARMUP undecided, no heat, no acknowledgment"
                   : "a warm loop: WARMUP undecided, no heat, no acknowledgment");
        L->dead_up = 0;
        /* The undecided hold judged no fan. The exhaust has not spun up when
         * the deciding tick writes its run duty: that tick permits fire and
         * must not acknowledge the window, which waits for the run's fans. */
        if (!cold)
            put_long("thermal/tach_exhaust", 10000000);     /* 3000 rpm: under the 6400 floor */
        run_tick(L);
        if (cold) {
            CHECK(cool_state == Cool_Warmup && !warmup_pending && heater_pct > 0 &&
                  !strcmp(pub_verdict, "WARMUP"), "the reading under the gate: the warm-up proper, heater on");
        } else {
            CHECK(cool_state == Cool_Run && !warmup_pending && get_long("thermal/exhaust_pwm") == EXHAUST_RUN,
                  "the reading over the gate: the run, its fans up");
            CHECK(pub_fire_ok && !pub_armed_ack,
                  "the deciding tick: fire permitted, the window not acknowledged");
            run_tick(L);
            run_tick(L);
            CHECK(!pub_armed_ack, "the exhaust still under its floor: no acknowledgment");
            put_long("thermal/tach_exhaust", 300);
            run_tick(L);
            CHECK(pub_armed_ack, "the exhaust at its floor: acknowledged");
        }
        kv[0] = NULL;
    }
    /* Undecided past the fans' spin-up grace (here 20 s blind): the run it
     * turns into gets a grace of its own, so a fan still spinning up is
     * not judged the tick it is told to run. */
    close_session(L);
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    L->dead_up = 1;
    for (int i = 0; i < 20; i++)
        run_tick(L);
    CHECK(cool_state == Cool_Warmup && warmup_pending, "20 s with no upstream reading: still undecided");
    L->dead_up = 0;
    put_long("thermal/tach_exhaust", 10000000);     /* 3000 rpm: the exhaust spinning up */
    int tripped = 0;
    for (int i = 0; i < 6; i++) {
        run_tick(L);
        tripped |= airflow_alarm;
    }
    CHECK(cool_state == Cool_Run && !tripped,
          "decided warm past the grace: the run's own grace, no AIRFLOW on a fan still spinning up");
    put_long("thermal/tach_exhaust", 300);

    /* A warm-up decided, or opened, while SENSOR stands: no heater write
     * in the tick, and the warm-up does not say "heater on" until it is. */
    kv[0] = "cool_temp_start"; kv[1] = "23.3"; kv[2] = NULL;    /* over the loop's 22.3 */
    for (int pass = 0; pass < 2; pass++) {
        close_session(L);
        for (int i = 0; i < 20; i++)
            run_idle_tick(L);
        if (pass == 0) {
            L->session = 1;
            L->dead_up = 1;                 /* undecided, then SENSOR */
        } else {
            L->dead_down = 1;               /* SENSOR at idle, then the session opens */
        }
        for (int i = 0; i < 3; i++)
            pass == 0 ? run_tick(L) : run_idle_tick(L);
        CHECK(sensor_alarm, pass == 0 ? "undecided and blind: SENSOR" : "blind at idle: SENSOR");
        L->dead_up = 0;
        L->session = 1;
        heater_written_max = 0;
        logbuf[0] = '\0';
        run_tick(L);
        CHECK(cool_state == Cool_Warmup && !warmup_pending && sensor_alarm && heater_written_max == 0 &&
              !log_has("heater on"),
              pass == 0 ? "decided cold under SENSOR: the warm-up, no heater write, not said on"
                        : "opened under the gate under SENSOR: the warm-up, no heater write, not said on");
        L->dead_down = 0;
        int told = 1;
        for (int i = 0; i < 4; i++) {
            run_tick(L);
            told &= warmup_reason_true();
        }
        CHECK(!sensor_alarm && heater_pct > 0 && strstr(pub_reason, "heater on") && told,
              "SENSOR released: the heater on, and the hold's reason says so");
    }
    /* A warm-up opened on a tick the downstream sensor gave no reading (one
     * tick, tolerated): the heater off that tick, on the next, and the
     * reason follows it. */
    close_session(L);
    for (int i = 0; i < 61; i++)
        run_idle_tick(L);
    L->session = 1;
    L->dead_down = 1;
    run_tick(L);
    CHECK(cool_state == Cool_Warmup && !sensor_alarm && heater_pct == 0 && strstr(pub_reason, "heater off"),
          "opened on a tick with no downstream reading: the heater off, and said");
    L->dead_down = 0;
    int told = 1;
    for (int i = 0; i < 3; i++) {
        run_tick(L);
        told &= warmup_reason_true();
    }
    CHECK(heater_pct > 0 && strstr(pub_reason, "heater on") && told,
          "the reading back: the heater on, and the hold's reason says so");
    kv[0] = NULL;
    close_session(L);
}

/* The lid IR fire watch with a channel unreadable: the three that read
 * are judged meanwhile, and two ticks in a row is SENSOR, released once all
 * four read. A count past 1023 is unreadable; 0 is a reading. */
static void ir_blind(loop_t *L)
{
    printf("O5. a lid IR channel unreadable in a run: SENSOR at the second tick; the others judged\n");
    close_session(L);
    cool_notices_dismiss((unsigned)-1);
    lid_ir_all(100);
    settle_and_open(L);
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "a settled session: fire permitted");
    unlink(GF_SYSFS "pic/lid_ir_2");
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "one tick unread: tolerated");
    run_tick(L);
    CHECK(!pub_fire_ok && pub_hold && !strcmp(pub_verdict, "SENSOR") && strstr(pub_reason, "lid IR channel 2"),
          "two ticks: SENSOR, fire blocked, hold, the channel named");
    put_long("pic/lid_ir_2", 100);
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "all four read again: released");
    put_long("pic/lid_ir_1", 5000);     /* past the converter: no reading */
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && strstr(pub_reason, "lid IR channel 1"),
          "a count past 1023: unreadable, SENSOR");
    lid_ir_all(0);
    run_tick(L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "four channels at 0: readings, released");
    void (*hook_was)(const char *) = cool_fail_tier_stop;
    cool_fail_tier_stop = fake_stop;
    stops = 0;
    unlink(GF_SYSFS "pic/lid_ir_4");
    for (int i = 1; i <= 3; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 800);               /* the three that read past q1's critical */
    }
    run_tick(L);
    run_tick(L);
    CHECK(!strcmp(pub_verdict, "FIRE") && stops == 1, "the three that read are judged: FIRE, the controller ended");
    cool_fail_tier_stop = hook_was;
    for (int i = 1; i <= 3; i++) {
        char a[24];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        put_long(a, 100);
    }
    CHECK(ir_blind_alarm, "channel 4 still unread: SENSOR stands under the FIRE");
    close_session(L);
    run_idle_tick(L);
    CHECK(!ir_blind_alarm, "the watch stood down at idle, the channel still unread");
    put_long("pic/lid_ir_4", 100);
    /* 1023 is the converter's full scale, a reading: a saturated lid is
     * FIRE through both quartiles' critical tiers (688, 1022), not SENSOR. */
    for (int three = 0; three <= 1; three++) {
        hook_was = cool_fail_tier_stop;
        cool_fail_tier_stop = fake_stop;
        stops = 0;
        lid_ir_all(100);
        settle_and_open(L);
        lid_ir_all(1023);
        if (three)
            unlink(GF_SYSFS "pic/lid_ir_2");
        run_tick(L);
        run_tick(L);
        CHECK(!strcmp(pub_verdict, "FIRE") && stops == 1 && !strcmp(pub_fire_watch, "ALARM"),
              three ? "three channels at 1023, one unread: FIRE" : "four channels at 1023: FIRE, not SENSOR");
        cool_fail_tier_stop = hook_was;
        lid_ir_all(100);
        close_session(L);
        run_idle_tick(L);
    }
    cool_notices_dismiss((unsigned)-1);
}

/* One run tick with a coolant sensor (upstream unless the case names the
 * downstream one) at the rail (blind) or reading. */
static const char *rail_attr = "pic/water_temp_2";
static void rail_tick(loop_t *L, int blind)
{
    loop_tick(L);
    if (blind)
        put_long(rail_attr, 1023);
    fake_now += 1.0;
    cool_state_report("run", 1, -1, -1, -1, NULL);
    engine_tick();
}

/* A minute and a second of readings: no tolerated tick left in the window. */
static void wash_tolerated(loop_t *L)
{
    for (int i = 0; i < 61; i++)
        rail_tick(L, 0);
}

static int said_count(const char *what)
{
    int n = 0;
    for (const char *s = logbuf; (s = strstr(s, what)) != NULL; s++)
        n++;
    return n;
}

/* A daemon start with the settings file unreadable: the engine has never
 * read ext_enabled, and a package's hold stands. Run in a child, so the
 * engine's statics are as a fresh process has them. */
static void ext_unknown_at_start(void)
{
    printf("P0. a fresh engine, the settings file unreadable before its first read\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        mkdir("cool-engine-test/holds", 0755);
        holds_dir = "cool-engine-test/holds";
        mkdir("cool-engine-test/required-holds", 0755);
        holds_required_dir = "cool-engine-test/required-holds";
        ext_safe_file = "cool-engine-test/ext-safe";
        unlink(ext_safe_file);
        settings_unreadable = 1;
        loop_t L = { 0 };
        L.fix_down = L.fix_up = -1;
        open_session(&L);
        ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
        run_tick(&L);
        CHECK(!strcmp(pub_verdict, "EXT") && !pub_fire_ok && pub_hold,
              "never read: the hold stands");
        unlink("cool-engine-test/holds/org.example.badge.json");
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the fresh engine's case passed");
}

/* AIRFLOW and CRITICAL take the kernel pair on their rising edge: cnc/stop
 * and the engine's latch hold, written before the words, once; the hold
 * stands for the session and is released at its end. The controller is not
 * ended through the supervisor: it ends the job itself on the verdict. */
static const char *pair_words;
static long pair_stop_at_words, pair_latch_at_words;
static void pair_hook(const char *line)
{
    if (pair_words && strstr(line, pair_words) && pair_stop_at_words == -2) {
        pair_stop_at_words = get_long("cnc/stop");
        pair_latch_at_words = get_long("cnc/laser_latch_cooling");
    }
}

static void fail_tier_pair(loop_t *L, int critical)
{
    printf("N2%s. %s: the kernel pair on the rising edge, before the words; no controller stop\n",
           critical ? "b" : "a", critical ? "CRITICAL" : "AIRFLOW");
    close_session(L);
    if (critical) {
        kv[0] = "cool_temp_max"; kv[1] = "60"; kv[2] = "cool_temp_resume"; kv[3] = "16";
        kv[4] = "cool_temp_critical_c"; kv[5] = "30"; kv[6] = NULL;
    }
    settle_and_open(L);
    {
        double t0 = fake_now;
        while (fake_now < t0 + FAN_GRACE_S + 1)
            run_tick(L);
    }
    CHECK(pub_armed_ack && pub_fire_ok, "an acknowledged window past the grace");
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
    stops = 0;
    pair_words = critical ? "CRITICAL:" : "AIRFLOW:";
    pair_stop_at_words = pair_latch_at_words = -2;
    fflog_hook = pair_hook;
    write_turn = stop_turn = cool_lock_turn = 0;
    int stop_before = 0, ticks = 0;
    if (critical)
        L->bulk_c = 10.0;                   /* 32.3 C: over the 30 C critical line */
    else
        put_long("thermal/tach_exhaust", 10000000);     /* 3000 rpm: under the 6400 floor */
    while (!(critical ? critical_alarm : airflow_alarm) && ticks < 10) {
        stop_before |= get_long("cnc/stop") != 0;
        run_tick(L);
        ticks++;
    }
    fflog_hook = NULL;
    CHECK(!strcmp(pub_verdict, critical ? "CRITICAL" : "AIRFLOW") && !pub_fire_ok && !stop_before &&
          ticks == (critical ? 1 : 3),
          critical ? "the critical line crossed: CRITICAL at that tick, nothing written before it"
                   : "the exhaust under its floor: AIRFLOW at the gate's third tick, nothing written before it");
    CHECK(pair_stop_at_words == 1 && pair_latch_at_words == 1,
          "motion stopped and the engine's latch hold locked before the words were logged");
    CHECK(stop_turn > 0 && cool_lock_turn > stop_turn, "cnc/stop written first, then the engine's latch hold");
    CHECK(stops == 0, "the controller is not ended through the supervisor");
    put("cnc/stop", "0\n");
    run_tick(L);
    run_tick(L);
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 1,
          "the tier stands: the stop written once, the engine's latch hold stands with it");
    /* The window closes and the session stays open (a paused, disarmed
     * cloud print reports run; an M8 left on): the hold stands. */
    for (int k = 0; k < 3; k++)
        tick_unarmed_run(L);
    CHECK(get_long("cnc/laser_latch_cooling") == 1 && !strcmp(pub_verdict, critical ? "CRITICAL" : "AIRFLOW"),
          "the window closed in the open session: the tier and the engine's latch hold stand");
    L->bulk_c = 0.0;
    put_long("thermal/tach_exhaust", 300);
    kv[0] = NULL;
    close_session(L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the tier ended with its session: the hold released");
    if (critical)
        return;

    /* A session no window ever opened in (an M8 job with no M3): AIRFLOW
     * takes the pair there too. */
    for (int i = 0; i < 20; i++)
        run_idle_tick(L);
    L->session = 1;
    {
        double t0 = fake_now;
        while (fake_now < t0 + FAN_GRACE_S + 1)
            tick_unarmed_run(L);
    }
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
    put_long("thermal/tach_exhaust", 10000000);
    ticks = 0;
    while (!airflow_alarm && ticks < 10) {
        tick_unarmed_run(L);
        ticks++;
    }
    CHECK(ticks == 3 && get_long("cnc/stop") == 1 && get_long("cnc/laser_latch_cooling") == 1 && stops == 0,
          "an unarmed session: AIRFLOW at the third low tick takes the pair, no controller stop");
    put_long("thermal/tach_exhaust", 300);
    close_session(L);

    /* A stop the kernel refuses: the latch lock lands all the same, and
     * the failed write is named. */
    settle_and_open(L);
    {
        double t0 = fake_now;
        while (fake_now < t0 + FAN_GRACE_S + 1)
            run_tick(L);
    }
    put("cnc/laser_latch_cooling", "0\n");
    logbuf[0] = '\0';
    drop_attr = "cnc/stop";
    refuse = 1;
    drop_left = -1;
    put_long("thermal/tach_exhaust", 10000000);
    for (int k = 0; k < 10 && !airflow_alarm; k++)
        run_tick(L);
    drop_attr = NULL;
    refuse = 0;
    drop_left = 0;
    CHECK(airflow_alarm && get_long("cnc/laser_latch_cooling") == 1 && !pub_fire_ok &&
          log_has("safing write cnc/stop=1 failed"),
          "the stop refused: the engine's latch hold locked all the same, the failed write named");
    put_long("thermal/tach_exhaust", 300);
    close_session(L);
}

int main(void)
{
    make_tree();
    ext_unknown_at_start();
    /* The engine's first tick, on the tree a final close of the pulse
     * device leaves (every holder of the laser latch holding). */
    printf("R0. a fresh engine releases the latch hold the final close left\n");
    tick("idle", 0);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the engine's own hold released at its first tick");
    /* The fan writes and the intakes fitted run on a loop at rest; the
     * tree is laid again after them. */
    fan_writes();
    intakes();
    make_tree();
    kv[0] = NULL;
    loop_t L = { 0 };
    L.fix_down = L.fix_up = -1;

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
    logbuf[0] = '\0';
    run_tick(&L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK") && log_has("tolerated"),
          "one failed read is tolerated, and said");
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
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR"), "the first reading back: SENSOR stands while the score decays");
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "SENSOR"), "the third reading back: still SENSOR");
    run_tick(&L);
    CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "the fourth reading in a row: released");
    L.dead_down = 1;
    run_tick(&L);
    run_tick(&L);
    CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR") && strstr(last_log, "downstream") != NULL,
          "the downstream sensor alone is enough to blind the engine");
    L.dead_down = 0;
    for (int i = 0; i < 4; i++)
        run_tick(&L);

    printf("H3. dropouts with readings between: the score, then the minute's count\n");
    {
        /* No reading, a reading, no reading: SENSOR at the second, released
         * at the third reading after it. */
        wash_tolerated(&L);
        rail_tick(&L, 1);
        rail_tick(&L, 0);
        rail_tick(&L, 1);
        CHECK(!pub_fire_ok && !strcmp(pub_verdict, "SENSOR"), "B G B: SENSOR at the second no-reading tick");
        rail_tick(&L, 0);
        rail_tick(&L, 0);
        CHECK(!strcmp(pub_verdict, "SENSOR"), "B G B, then two readings: still SENSOR");
        rail_tick(&L, 0);
        CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "B G B, then the third reading: released");

        int blocked = 0, sensor_at = -1;
        for (int i = 0; i < 20; i++) {
            rail_tick(&L, i % 2 == 0);
            blocked += !pub_fire_ok;
            if (sensor_at < 0 && !strcmp(pub_verdict, "SENSOR"))
                sensor_at = i;
        }
        CHECK(sensor_at == 2 && blocked == 18 && !strcmp(pub_verdict, "SENSOR"),
              "the rail on every other tick: SENSOR at the second no-reading tick, fire blocked from it on");
        for (int i = 0; i < 3; i++)
            rail_tick(&L, 0);
        CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "steady readings again: released");

        /* Too sparse for the score: the sixth tolerated tick in 60 s is
         * SENSOR, and the rate keeps it standing. */
        /* every third upstream, every fourth upstream, every third downstream */
        for (int run = 0; run < 3; run++) {
            int every = run == 1 ? 4 : 3;
            rail_attr = run == 2 ? "pic/water_temp_1" : "pic/water_temp_2";
            wash_tolerated(&L);
            logbuf[0] = '\0';
            int open_blind = 0;
            sensor_at = -1;
            for (int i = 0; i < 12 * every; i++) {
                int b = i % every == 0;
                rail_tick(&L, b);
                open_blind += b && pub_fire_ok;
                if (sensor_at < 0 && !strcmp(pub_verdict, "SENSOR"))
                    sensor_at = i;
            }
            CHECK(sensor_at == 5 * every && open_blind == 5 && !strcmp(pub_verdict, "SENSOR"),
                  run == 0 ? "the upstream rail on every third tick: five tolerated, SENSOR at the sixth, and it stands"
                  : run == 1 ? "the upstream rail on every fourth tick: five tolerated, SENSOR at the sixth"
                             : "the downstream rail on every third tick: five tolerated, SENSOR at the sixth, and it stands");
            CHECK(log_has("on 6 ticks in 60 s") && log_has(run == 2 ? "downstream sensor" : "upstream sensor"),
                  "the SENSOR line names the sensor and gives the minute's count");
            for (int i = 0; i < 4; i++)
                rail_tick(&L, 0);
            CHECK(pub_fire_ok && !strcmp(pub_verdict, "OK"), "steady readings: released by the score");
        }
        rail_attr = "pic/water_temp_2";

        /* An isolated glitch a minute apart: tolerated, said each time. */
        wash_tolerated(&L);
        logbuf[0] = '\0';
        int glitch_sensor = 0;
        for (int g = 0; g < 7; g++) {
            rail_tick(&L, 1);
            glitch_sensor += !strcmp(pub_verdict, "SENSOR");
            for (int i = 0; i < 59; i++)
                rail_tick(&L, 0);
        }
        CHECK(glitch_sensor == 0 && said_count("1 tolerated in the last 60 s") == 7,
              "a glitch every 60 s: never SENSOR, each one said with its count");

        /* The line is said at most once a minute, with the minute's count. */
        wash_tolerated(&L);
        logbuf[0] = '\0';
        rail_tick(&L, 1);
        for (int i = 0; i < 9; i++)
            rail_tick(&L, 0);
        rail_tick(&L, 1);
        CHECK(said_count("tolerated in the last") == 1, "a second tolerated tick 10 s after the first: not said");
        for (int i = 0; i < 49; i++)
            rail_tick(&L, 0);
        rail_tick(&L, 1);
        CHECK(said_count("tolerated in the last") == 2 && log_has("2 tolerated in the last 60 s"),
              "one 60 s after the first said: said, with the two of the last minute");
        for (int i = 0; i < 3; i++)
            rail_tick(&L, 0);
    }

    counts_no_reading(&L);

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
    int cold_blocked = 1;
    for (int i = 0; i < SENSOR_FAIL_CAP; i++) {
        run_tick(&L);
        cold_blocked &= !pub_fire_ok;
    }
    CHECK(cold_gate && cold_blocked && !strcmp(pub_verdict, "COLD"),
          "reading again: SENSOR, then still COLD, never a moment of fire");
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
    for (int i = 0; i < 4; i++)
        run_tick(&L);
    CHECK(cool_state == Cool_Warmup && !strcmp(pub_verdict, "WARMUP") && heater_pct > 0 &&
          strstr(pub_reason, "WARM-UP") && strstr(pub_reason, "heater on"),
          "reading again, SENSOR released: the warm-up holds on with its heater back, and says so");
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
    CHECK(cool_quiet_hold_try(1, 1) < 0 && !quiet_held, "the route's pump hold refused under a warm-up");
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
    CHECK(cool_quiet_hold_try(1, 1) == 0 && quiet_pump_held, "idle again: the route takes a pump hold");
    cool_quiet_hold(0);

    flow_yields_table(&L);
    flow_yield_now(&L);

    test_ack_waits_for_fans(&L);
    fans_under_the_fail_tiers(&L);
    quiet_blocks_fire(&L);
    button_reserved_case(&L);
    packages_case(&L);
    host_word_case(&L);
    window_own_ack_case(&L);

    /* The safing attributes the engine writes: present in the tree as
       "0" so a write can be seen, and put back to "0" between ticks so
       a repeat can be told from a single write. */
    printf("M. the hang dead-man: once while armed, every tick while the kernel runs\n");
    open_session(&L);
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
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
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 0 && pub_armed_ack &&
          !log_has("controller silent"),
          "4.5 s without a report: nothing safed, the window still acknowledged");
    fake_now += 1.5;                    /* 6.0 s: past REPORT_TIMEOUT_S with no report */
    int ev0 = events;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch_cooling") == 1,
          "silence while armed: motion stopped and the engine's own latch hold locked");
    CHECK(events == ev0 + 1, "and the flight recorder is asked for the minutes around it, once");
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
    fake_now += 1.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch_cooling") == 0,
          "the armed case writes once per silence");
    put("cnc/laser_latch_cooling", "1\n");  /* the kernel's: the engine holds */
    fake_now += 1.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/laser_latch_cooling") == 1, "the hold is not released while the silence lasts");
    put("cnc/state", "running\n");
    for (int i = 0; i < 3; i++) {
        put("cnc/stop", "0\n");
        put("cnc/laser_latch_cooling", "0\n");
        fake_now += 1.0;
        loop_tick(&L);
        engine_tick();
        CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch_cooling") == 1,
              "the kernel still running: the pair is written again this tick");
    }
    /* A state that cannot be read counts as running for 5 s, then as not
     * running, said once. */
    unlink(GF_SYSFS "cnc/state");
    for (int i = 0; i < 4; i++) {
        put("cnc/stop", "0\n");
        put("cnc/laser_latch_cooling", "0\n");
        fake_now += 1.0;
        loop_tick(&L);
        engine_tick();
        CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch_cooling") == 1,
              "the kernel's state unreadable: counted running, the pair written again");
    }
    logbuf[0] = '\0';
    for (int i = 0; i < 3; i++) {
        put("cnc/stop", "0\n");
        fake_now += 1.0;
        loop_tick(&L);
        engine_tick();
    }
    const char *named = strstr(logbuf, "taken as not running");
    CHECK(get_long("cnc/stop") == 0 && named && !strstr(named + 1, "taken as not running"),
          "unreadable past 5 s: taken as not running, said once");
    put("cnc/state", "idle\n");
    /* A report again: the silence is over, and the hold is released that
     * tick, before the tick's verdict. */
    run_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0 && latch_at_verdict == 0,
          "a fresh report: the hold released, and released before the verdict was written");
    close_session(&L);

    /* The state is read every tick, so its 5 s run on wall time: a later
     * stretch of failed reads gets its own, and a stretch that lasts is
     * not running whoever asks. */
    printf("M2. cnc/state unreadable: the 5 s run on wall time\n");
    put("cnc/state", "idle\n");
    for (int i = 0; i < 3; i++)
        run_idle_tick(&L);
    unlink(GF_SYSFS "cnc/state");
    put("cnc/stop", "0\n");
    fake_now += 6.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 1, "silence 1, the state unreadable: counted running, stop written");
    put("cnc/state", "idle\n");
    for (int i = 0; i < 100; i++)
        run_idle_tick(&L);
    unlink(GF_SYSFS "cnc/state");
    put("cnc/stop", "0\n");
    fake_now += 6.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 1,
          "silence 2, the state readable between: a stretch of its own, counted running, stop written");
    for (int i = 0; i < 100; i++)
        run_idle_tick(&L);
    put("cnc/stop", "0\n");
    fake_now += 6.0;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 0,
          "silence 3, the state unreadable all the 100 s before it: not running, no stop");
    put("cnc/state", "idle\n");
    for (int i = 0; i < 3; i++)
        run_idle_tick(&L);

    /* The thermal cooldown asks at its phase's end, 300 s apart: the state
     * unreadable throughout is taken as not running 5 s in, and the phase
     * times out. */
    printf("M3. cnc/state unreadable through a thermal cooldown: it times out\n");
    {
        double bulk_was = L.bulk_c;
        L.bulk_c = 10.0;        /* upstream about 32 C: over the 31 C resume gate, under the 33 C ceiling */
        open_session(&L);
        for (int i = 0; i < 5; i++)
            run_tick(&L);
        unlink(GF_SYSFS "cnc/state");
        L.session = 0;
        int left = -1, thermal_seen = 0, said_at = -1;
        logbuf[0] = '\0';
        for (int i = 0; i < 3000; i++) {
            loop_tick(&L);
            fake_now += 1.0;
            cool_state_report("idle", 0, -1, -1, -1, NULL);
            engine_tick();
            thermal_seen |= cool_state == Cool_Thermal;
            if (said_at < 0 && log_has("taken as not running"))
                said_at = i;
            if (cool_state == Cool_Idle) {
                left = i;
                break;
            }
        }
        CHECK(said_at >= 0 && said_at <= 5, "taken as not running 5 s after the first failed read");
        CHECK(thermal_seen && left >= 0 && left < 400 && log_has("timed out"),
              "the thermal phase times out on its 300 s, and the engine is idle");
        put("cnc/state", "idle\n");
        L.bulk_c = bulk_was;
        close_session(&L);
    }

    diag_heater_lease(&L);

    printf("N. the fail tiers end the controller through the supervisor, once\n");
    cool_fail_tier_stop = fake_stop;
    stops = 0;
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
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
    CHECK(get_long("cnc/laser_latch_cooling") == 1, "and the engine's latch hold stands with it");
    for (int i = 1; i <= 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i);
        put_long(attr, 100);
    }
    close_session(&L);
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the tier ended with its session: the hold released");
    stops = 0;
    put("cnc/stop", "0\n");
    put("cnc/laser_latch_cooling", "0\n");
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
    CHECK(get_long("cnc/laser_latch_cooling") == 1, "and the engine's latch hold stands with it");
    CHECK(attn_latched && attn_sticky && !strcmp(attn_verdict, "CRASH"),
          "CRASH is the fail tier: it takes the acknowledgment to end it");
    crash_src2 = 0;
    close_session(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the tier ended with its session: the hold released");
    fail_tier_pair(&L, 0);
    fail_tier_pair(&L, 1);

    /* The kernel sets the cooling holder at every final close of the pulse
     * device, long after the engine's first tick (a controller that
     * outlived a daemon restart closing the device later): with no reason
     * standing, the engine releases it at its next tick, and a release the
     * kernel did not take is written again the tick after. */
    printf("R. a hold a later final close sets is released at the next tick with no reason\n");
    put("cnc/laser_latch_cooling", "1\n");      /* a later final close */
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the hold a later final close left is released");
    put("cnc/laser_latch_cooling", "1\n");
    drop_attr = "cnc/laser_latch_cooling";
    drop_left = -1;
    refuse = 1;
    int errs0 = log_errs;
    run_idle_tick(&L);
    run_idle_tick(&L);
    CHECK(log_errs == errs0 + 1, "two releases the kernel did not take are named once");
    drop_attr = NULL;
    drop_left = 0;
    refuse = 0;
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "a release that did not take is written again the next tick");
    put("cnc/laser_latch_cooling", "1\n");
    drop_attr = "cnc/laser_latch_cooling";
    drop_left = -1;
    refuse = 1;
    run_idle_tick(&L);
    CHECK(log_errs == errs0 + 2, "a refusal after a release that took is named again");
    drop_attr = NULL;
    drop_left = 0;
    refuse = 0;
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "and the hold is released once the kernel takes it");

    /* The engine's lock writes the controller's hold too, after its own:
     * on a module with the cooling holder the second write changes
     * nothing, and on one without it the latch still locks. Emission with
     * no armed window is the lock (the armed window is long past). */
    printf("S. the engine's lock locks the controller's hold too, after its own\n");
    put("cnc/laser_latch_cooling", "0\n");
    put("cnc/laser_latch", "0\n");
    cooling_at_ctl_lock = -2;
    put_long("cnc/laser_on_sampled", 5);
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 1 && get_long("cnc/laser_latch") == 1,
          "both holds locked");
    CHECK(cooling_at_ctl_lock == 1, "the controller's hold written after the engine's own");
    put_long("cnc/laser_on_sampled", 0);
    run_idle_tick(&L);
    unlink(GF_SYSFS "cnc/laser_latch_cooling");     /* a module without the cooling holder */
    put("cnc/laser_latch", "0\n");
    put_long("cnc/laser_on_sampled", 5);
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch") == 1,
          "on a module without the cooling holder the latch is still locked");
    put_long("cnc/laser_on_sampled", 0);
    put("cnc/laser_latch_cooling", "1\n");
    run_idle_tick(&L);
    CHECK(get_long("cnc/laser_latch_cooling") == 0, "the holder back: the hold released");

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
    ir_blind(&L);

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
    /* A settings file that cannot be read is not extensions off. */
    settings_unreadable = 1;
    ext_hold_file("org.example.badge", 1, 1, "no badge presented", fake_now + 1.0);
    run_tick(&L);
    settings_unreadable = 0;
    CHECK(!strcmp(pub_verdict, "EXT") && !pub_fire_ok && pub_hold,
          "the settings file unreadable for a tick: the hold stands");
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
          json_is_boolean(json_object_get(doc, "button_reserved")) &&
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
    warmup_undecided(&L);
    gate_critical(&L);
    gate_fans(&L);
    crash_watch(&L);
    crash_watch_stands_down(&L);
    flame_over_session_open(&L);
    nonfinite_tunables(&L);
    tec(&L);
    outputs_every_tick(&L);
    quiet_posture(&L);
    cold_side(&L);
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
