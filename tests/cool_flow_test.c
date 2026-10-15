/*
 * cool_flow_test.c - host unit test for the cooling engine, tick by tick
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The flow check heats the loop for a window and judges the downstream
 * rise. Two things the bench found put an ordinary job's check within
 * tenths of the limit: the coolant ADC's common-mode offset (about 1 C,
 * stepping in with the run airflow and toggling between two levels), and
 * the tube's own heat (about 1.5 C over a lit CW window). The engine now
 * reads the baseline and the end as means and takes the tube's share off
 * the rise, bounded. This test drives the engine tick by tick against a
 * fake sysfs tree and a fake clock, with a loop model that carries the
 * offset, the heater's rise (flow or no flow) and the tube's lagged heat:
 *
 *   A. dark, flow:          verified, no share taken
 *   B. dark, no flow:       SUSPECT
 *   C. lit CW, flow:        verified, the share about 1.5 C
 *   D. lit CW, no flow:     SUSPECT (the share never masks a stopped pump)
 *   E. absurd coefficient:  the share is bounded, no flow still SUSPECT
 *   F. density model:       the density coefficient applies (0.77 of CW)
 *
 * The same harness drives the verdict a coolant sensor read failure
 * produces (a sysfs error, not a rail value): the engine is blind on
 * every coolant gate, so it says so rather than fire on:
 *
 *   H. blind mid-run:       one failed read tolerated, two are SENSOR
 *                           (fire blocked, hold, heater off), released
 *                           the moment the reading is back
 *   I. blind in a check:    the heater goes off, the check is abandoned
 *                           and asked for again once the sensors read
 *   J. blind under the floor: COLD keeps its state through SENSOR
 *   K. blind in a warm-up:  the heater goes off and comes back
 *   M. the hang dead-man:   a report under the timeout is still fresh and
 *                           acknowledged; silence while armed stops and
 *                           locks once; silence while the kernel runs
 *                           repeats every tick
 *   N. the fail tiers:      FIRE and CRASH end the controller through the
 *                           supervisor's hook, once, after the kernel writes
 *   O. the pause tiers:     FLAME and BUMP hold the job and call nothing
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
 *              60 it is off, said so; the run's end names the board temps
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
 */
#define GF_SYSFS    "cool-flow-test/sys/"
#define VERDICT_DIR "cool-flow-test/run"
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

int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
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
    snprintf(path, sizeof(path), "cool-flow-test/holds/%s.json", id);
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

/* The faked crash watch hardware: armed on request, and the next poll
   reports what the test put in src1/src2. */
static int crash_armed_fake;
static int crash_arm_calls;
static unsigned crash_src1, crash_src2;
int fake_crash_open(void) { return 0; }
int fake_crash_arm(int x, int y, int abort)
{
    (void)x; (void)y; (void)abort;
    crash_armed_fake = 1;
    crash_arm_calls++;
    return 0;
}
int fake_crash_poll(unsigned *s1, unsigned *s2) { *s1 = crash_src1; *s2 = crash_src2; return 0; }
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
double chassis_degc(void) { return 25.0; }
long supply_temp_raw(void) { return 540; }
double soc_degc(void) { return 38.0; }
long soc_throttle_state(void) { return 0; }

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
    mkdir("cool-flow-test", 0755);
    mkdir("cool-flow-test/run", 0755);
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
    int flow;                       /* pump running */
    int lit;                        /* tube at full current from the open */
    int dead_down, dead_up;         /* a sensor whose read fails */
    double k_true;                  /* the tube's real C per raw-second */
    int session;                    /* ticks since the session opened */
    int heater_ticks;               /* ticks the heater has been on */
    double tube_c;                  /* tube heat arrived at the sensor */
    double bulk_c;                  /* the bulk warmed past its base (a warm-up) */
} loop_t;

static void loop_tick(loop_t *L)
{
    /* The engine's own view of the heater: the fake tree is not sysfs,
       and a short write does not truncate what a longer one left. */
    long heater = flow_check_active ? 1 : 0;
    if (heater > 0)
        L->heater_ticks++;
    else
        L->heater_ticks = 0;
    double th = L->heater_ticks ? (double)(L->heater_ticks - 1) : 0.0;
    double heater_c = 0.0;
    if (L->heater_ticks)
        heater_c = L->flow ? 12.0 * (1.0 - exp(-th / 12.0)) : 0.4 * th;
    /* The tube's heat arrives 15 s after emission. */
    long hv = L->lit && L->session > 0 ? 970 : 0;
    if (L->lit && L->session > 15)
        L->tube_c += L->k_true * 970.0;
    /* The run airflow's ADC offset: in from the session open, toggling. */
    double offset = L->session > 0 ? -1.0 + ((L->session & 1) ? 0.6 : 0.0) : 0.0;
    double down = BASE_C + L->bulk_c + offset + heater_c + L->tube_c;
    double up = BASE_C + L->bulk_c + offset + 0.07 * heater_c + L->tube_c;
    if (L->dead_down)
        unlink(GF_SYSFS "pic/water_temp_1");
    else
        put_long("pic/water_temp_1", raw_for(down));
    if (L->dead_up)
        unlink(GF_SYSFS "pic/water_temp_2");
    else
        put_long("pic/water_temp_2", raw_for(up));
    put_long("pic/hv_current", hv);
    if (L->session > 0)
        L->session++;
}

/* --- driving the engine ----------------------------------------------- */

/* Close the session the way a job's end does: report idle and tick
   through the cooldown phases until the engine is idle with the heater
   off, then clear the verdict a previous case left. */
static void close_session(loop_t *L)
{
    L->session = 0;
    L->lit = 0;
    for (int i = 0; i < 900; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
        if (cool_state == Cool_Idle && !flow_check_active)
            break;
    }
    flow_verdict = Flow_Normal;
    flow_episodes = 0;
    flow_check_pending = 0;
    heater_set_pct(0);
    conf_reload();
}

/* Open a run session and tick until the check has run and judged.
   Returns the verdict; the engine's line is in last_log. */
static flow_verdict_t run_check(loop_t *L, int lit, int max_ticks)
{
    close_session(L);
    L->lit = lit;
    L->heater_ticks = 0;
    L->tube_c = 0.0;
    /* A settled loop before the job: history for the gate. */
    for (int i = 0; i < 20; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
    }
    L->session = 1;
    int saw_check = 0;
    for (int i = 0; i < max_ticks; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("run", 1, -1, -1, -1, NULL);
        engine_tick();
        if (flow_check_active)
            saw_check = 1;
        else if (saw_check)
            return flow_verdict;
    }
    printf("    (the check never completed in %d ticks; active=%d pending=%d)\n",
           max_ticks, flow_check_active, flow_check_pending);
    return Flow_Fault;
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
    L->lit = 0;
    L->heater_ticks = 0;
    L->tube_c = 0.0;
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
    logbuf[0] = '\0';
    session_end_tick(L);
    const char *t = strstr(logbuf, "temps this job");
    CHECK(t && strstr(t, "chassis 25.0") && strstr(t, "soc 38.0"), "the run's end names the board temperatures");
    kv[0] = NULL;
    close_session(L);
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
    session_end_tick(L);
    CHECK(!strcmp(pub_verdict, "OK") && !pub_hold, "the session over: OK, no hold");
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

int main(void)
{
    make_tree();
    /* The coefficient is selected by the model the controller reports
     * (the config key is gone; density is the only product model, and
     * the analog value survives for the host-test reference builds). */
    cool_state_model(0);
    /* The fan writes and the intakes fitted run on a loop at rest; the
     * flow check's tree is laid again after them. */
    fan_writes();
    intakes();
    make_tree();
    kv[0] = NULL;
    loop_t L = { .flow = 1, .lit = 0, .k_true = LASER_HEAT_CW };

    printf("A. dark, flow\n");
    flow_verdict_t v = run_check(&L, 0, 200);
    CHECK(v == Flow_Normal, "a dark check with flow is verified");
    CHECK(strstr(last_log, "laser") == NULL, "no share is taken with the tube dark");
    CHECK(flow_base_set && fabs(flow_base_down - (BASE_C - 0.7)) < 0.15,
          "the baseline is the mean of the settled window, not one sample");

    printf("B. dark, no flow\n");
    L.flow = 0;
    v = run_check(&L, 0, 200);
    CHECK(v == Flow_Suspect, "a dark check without flow is SUSPECT");

    printf("C. lit CW from the session open, flow\n");
    L.flow = 1;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Normal, "a lit check with flow is verified");
    CHECK(flow_laser_c > 1.2f && flow_laser_c < 1.8f,
          "the tube's share taken off is about 1.5 C");
    CHECK(strstr(last_log, "laser") != NULL, "the line carries the share");

    printf("D. lit CW, no flow\n");
    L.flow = 0;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Suspect, "a lit check without flow is still SUSPECT");

    printf("E. an absurd coefficient cannot subtract the check away\n");
    kv[0] = "cool_laser_heat_cw"; kv[1] = "1e-3"; kv[2] = NULL;
    v = run_check(&L, 1, 200);
    CHECK(flow_laser_c <= LASER_HEAT_MAX_C + 0.001f, "the share is bounded");
    CHECK(v == Flow_Suspect, "no flow is still SUSPECT under the bound");
    kv[0] = NULL;

    printf("F. the density model takes its own coefficient\n");
    cool_state_model(1);
    L.flow = 1;
    L.k_true = LASER_HEAT_DENSITY;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Normal, "a lit density check with flow is verified");
    CHECK(flow_laser_c > 0.9f && flow_laser_c < 1.4f,
          "the share is the density coefficient's (0.77 of CW)");

    printf("G. the air-assist offset is taken off in counts while the fan runs\n");
    /* The fan's ground lift raises the raw counts, and more counts read
       colder. The model writes raw counts with no such lift in them, so
       with the setting at 20 the engine must read both sensors 20 counts
       LOWER than the raw (warmer) while the run profile is on, and the raw
       when it is not. The flow check must not care (both ends shift
       alike). */
    kv[0] = "cool_aa_offset_counts"; kv[1] = "20"; kv[2] = NULL;
    L.flow = 1;
    L.lit = 0;
    v = run_check(&L, 0, 200);
    CHECK(v == Flow_Normal, "the check still verifies flow with the offset setting in force");
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

    printf("H. a coolant sensor that stops reading mid-run\n");
    cool_state_model(0);
    L.flow = 1;
    L.lit = 0;
    L.k_true = LASER_HEAT_CW;
    v = run_check(&L, 0, 200);
    CHECK(v == Flow_Normal && pub_fire_ok && !strcmp(pub_verdict, "OK"),
          "the loop verified: fire permitted");
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

    printf("H2. a held flow check does not start; the release lets it\n");
    close_session(&L);
    for (int i = 0; i < 5; i++)
        run_tick(&L);
    open_session(&L);           /* the run's start requests a check; the hold keeps it pending */
    cool_flow_check_hold(1);
    for (int i = 0; i < 60 && !flow_check_active; i++)
        run_tick(&L);
    CHECK(!flow_check_active && heater_pct == 0 && flow_check_pending,
          "the check stays pending under the hold");
    CHECK(pub_fire_ok, "the hold is not a gate: fire stays permitted");
    cool_flow_check_hold(0);
    for (int i = 0; i < 200 && !flow_check_active; i++)
        run_tick(&L);
    CHECK(flow_check_active && heater_pct > 0, "the released check starts");
    close_session(&L);
    for (int i = 0; i < 30; i++)
        run_tick(&L);
    cool_flow_check_hold(1);
    fake_now += COOL_FLOW_HOLD_MAX_S + 5;
    run_tick(&L);
    CHECK(!flow_check_held, "a forgotten hold runs out of time");

    printf("I. blind during a flow check: the heater goes off, the check is asked for again\n");
    open_session(&L);
    for (int i = 0; i < 200 && !flow_check_active; i++)
        run_tick(&L);
    CHECK(flow_check_active && heater_pct > 0,
          "a check is under way with the heater on");
    L.dead_down = 1;
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && !flow_check_active && heater_pct == 0,
          "blind: SENSOR, the check abandoned, the heater off");
    L.dead_down = 0;
    run_tick(&L);
    printf("    verdict=%s pending=%d active=%d heater=%u\n", pub_verdict,
           flow_check_pending, flow_check_active, (unsigned)heater_pct);
    CHECK(!strcmp(pub_verdict, "OK") && flow_check_pending,
          "reading again: OK, and the check is asked for again");
    {
        int saw = 0;
        flow_verdict_t vv = Flow_Fault;
        for (int i = 0; i < 300; i++) {
            run_tick(&L);
            if (flow_check_active)
                saw = 1;
            else if (saw) {
                vv = flow_verdict;
                break;
            }
        }
        CHECK(saw && vv == Flow_Normal, "the re-asked check runs and verifies flow");
    }

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
    open_session(&L);
    CHECK(cool_state == Cool_Warmup && heater_pct > 0 && !strcmp(pub_verdict, "WARMUP"),
          "the session opens under the start gate: warm-up, heater on");
    L.dead_up = 1;
    run_tick(&L);
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "SENSOR") && heater_pct == 0,
          "blind: SENSOR, the heater off");
    L.dead_up = 0;
    run_tick(&L);
    CHECK(cool_state == Cool_Warmup && !strcmp(pub_verdict, "WARMUP") && heater_pct > 0,
          "reading again: the warm-up holds on with its heater back");
    kv[0] = NULL;
    close_session(&L);

    test_ack_waits_for_fans(&L);

    /* The safing attributes the engine writes: present in the tree as
       "0" so a write can be seen, and put back to "0" between ticks so
       a repeat can be told from a single write. */
    printf("M. the hang dead-man: once while armed, every tick while the kernel runs\n");
    open_session(&L);
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    logbuf[0] = '\0';
    /* The tolerance side first: a report 4.5 s old is still fresh, so an
     * engine stalled under that (its last report up to a second old when
     * it stopped) comes back to an armed window it still acknowledges,
     * and the controller's own stale-verdict hold (2 s) resumes with no
     * press. */
    fake_now += 4.5;
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 0 && get_long("cnc/laser_latch") == 0 && pub_armed_ack &&
          !log_has("controller silent"),
          "4.5 s without a report: nothing safed, the window still acknowledged");
    fake_now += 1.5;                    /* 6.0 s: past REPORT_TIMEOUT_S with no report */
    loop_tick(&L);
    engine_tick();
    CHECK(get_long("cnc/stop") == 1 && get_long("cnc/laser_latch") == 1,
          "silence while armed: motion stopped and the latch locked");
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
    crash_src2 = 0;
    close_session(&L);

    printf("O. the pause tiers hold the job and call nothing\n");
    stops = 0;
    open_session(&L);
    crash_src1 = 0x40 | 0x08;                       /* IG1: the alert generator, Y */
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "BUMP") && stops == 0, "the alert generator: BUMP, no stop");
    crash_src1 = 0;
    for (int i = 0; i < 6; i++)
        run_tick(&L);
    CHECK(!strcmp(pub_verdict, "OK") && stops == 0, "quiet polls release the bump, no stop");
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
    mkdir("cool-flow-test/holds", 0755);
    holds_dir = "cool-flow-test/holds";
    mkdir("cool-flow-test/required-holds", 0755);
    holds_required_dir = "cool-flow-test/required-holds";
    ext_safe_file = "cool-flow-test/ext-safe";
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
    unlink("cool-flow-test/holds/org.example.badge.json");
    ext_hold_file("org.example.filter", 0, 1, "filter life under 5 percent", fake_now + 1.0);
    crash_src2 = 0x40 | 0x02;
    run_tick(&L);
    CHECK(!strcmp(pub_verdict, "CRASH") && stops == 1, "CRASH under a hold: CRASH, and the controller is ended");
    crash_src2 = 0;
    unlink("cool-flow-test/holds/org.example.filter.json");
    kv[0] = NULL;
    close_session(&L);
    cool_fail_tier_stop = NULL;

    phases(&L);
    gate_ceiling(&L);
    gate_warmup(&L);
    gate_critical(&L);
    gate_fans(&L);
    crash_watch(&L);
    tec(&L);

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the flow check reads means, takes the tube's share off, "
                      "bounds it, still sees a stopped pump under a lit tube, takes "
                      "the air-assist offset off while the fan runs, and says SENSOR "
                      "rather than fire blind; every fan duty is read back and held, "
                      "and one intake fan fitted leaves the second tach unjudged; the "
                      "gates trip past the loop and are off at their far ends, the "
                      "phases end at their duties, and the TEC keeps its rules\n",
           failures);
    return failures ? 1 : 0;
}
