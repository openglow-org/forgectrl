/*
 * cool_intake_test.c - host unit test for the intake fans fitted
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Some machines are built with one intake fan, and the second intake
 * tach then never reads a pulse. cool_intake_fans 1 (the setup's
 * airflow check writes it) takes that tach out of the airflow gates;
 * nothing else changes. This test drives the engine tick by tick
 * against a fake sysfs tree whose second intake tach reads nothing:
 *
 *   A. two fans:  the default judges the silent tach, trips AIRFLOW on
 *                 it, and never acknowledges the armed window
 *   B. one fan:   no fault, the window is acknowledged, and intake_2
 *                 is published as not fitted with no floor
 *   C. one fan:   a silent intake 1 still trips AIRFLOW on intake_1
 *   D. one fan:   with the intake gate off, no warning names intake_2
 *   E. any other value (a hand-edited 0) keeps intake_2 judged
 */
#define GF_SYSFS    "cool-intake-test/sys/"
#define VERDICT_DIR "cool-intake-test/run"
#define clock_gettime fake_clock_gettime
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* The fake tree is plain files: a write truncates, as a sysfs store
 * replaces. */
static int fake_open(const char *path, int flags, ...)
{
    return openat(AT_FDCWD, path, (flags & O_WRONLY) ? (flags | O_TRUNC) : flags, 0644);
}
#define open fake_open

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

/* The settings a leg sets; NULL is unset, the default. */
static const char *set_intake_fans;
static const char *set_intake_min;

int settings_get(const char *key, char *val, size_t len)
{
    const char *v = !strcmp(key, "cool_intake_fans") ? set_intake_fans
                  : !strcmp(key, "cool_tach_intake_min_rpm") ? set_intake_min
                  : NULL;
    if (!v)
        return -1;
    snprintf(val, len, "%s", v);
    return 0;
}

static char logbuf[16384];          /* every line since the last settle */

void fflog(int prio, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    (void)prio;
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof(logbuf))
        snprintf(logbuf + n, sizeof(logbuf) - n, "%s\n", line);
    printf("    log: %s\n", line);
}

static int log_has(const char *needle)
{
    return strstr(logbuf, needle) != NULL;
}

int diag_running(void) { return 0; }
int status_json(char *buf, size_t len) { (void)buf; (void)len; return 0; }
int machine_is_idle(void) { return 1; }
int setup_flag(const char *id, const char *reason)
{
    (void)id; (void)reason;
    return 0;
}
double chassis_degc(void) { return 25.0; }
long supply_temp_raw(void) { return 540; }
double soc_degc(void) { return 38.0; }
long soc_throttle_state(void) { return 0; }

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

/* Tach periods in ns at 2 pulses/rev: 7200000 is about 4170 rpm, an
 * intake at its run duty on the bench reference; 0 is a tach that saw
 * no pulse, which is what the kernel reads with no fan on it. */
#define TACH_RUN    7200000
#define TACH_NONE   0

static void make_tree(void)
{
    mkdir("cool-intake-test", 0755);
    mkdir("cool-intake-test/run", 0755);
    mkdir(GF_SYSFS, 0755);
    mkdir(GF_SYSFS "pic", 0755);
    mkdir(GF_SYSFS "thermal", 0755);
    mkdir(GF_SYSFS "head", 0755);
    mkdir(GF_SYSFS "cnc", 0755);
    put("cnc/state", "idle\n");
    put_long("cnc/faults", 0);
    put_long("cnc/laser_on_sampled", 0);
    put_long("cnc/laser_pgood_sampled", 255);
    put_long("thermal/tach_exhaust", 300);
    put_long("thermal/tach_intake_1", TACH_RUN);
    put_long("thermal/tach_intake_2", TACH_NONE);
    put_long("head/air_assist_tach", 300);
    put_long("head/purge_air_current", 629);
    put_long("thermal/heater_pwm", 0);
    put_long("pic/hv_current", 0);
    put_long("pic/water_temp_1", raw_for(22.3));
    put_long("pic/water_temp_2", raw_for(22.3));
    put_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    put_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    put_long("thermal/intake_pwm", INTAKE_IDLE);
}

/* --- driving the engine ----------------------------------------------- */

static void tick(const char *mode, int armed)
{
    fake_now += 1.0;
    cool_state_report(mode, armed, -1, -1, -1, NULL);
    engine_tick();
}

/* Close any session and settle idle; the next session reloads the
 * settings a leg set. */
static void settle(void)
{
    for (int i = 0; i < 400; i++) {
        tick("idle", 0);
        if (cool_state == Cool_Idle)
            break;
    }
    put_long("thermal/tach_intake_1", TACH_RUN);
    put_long("thermal/tach_intake_2", TACH_NONE);
    set_intake_fans = NULL;
    set_intake_min = NULL;
    logbuf[0] = '\0';
}

/* An armed session, run until it faults or for `ticks` ticks. */
static void armed_session(int ticks)
{
    for (int i = 0; i < ticks && !airflow_alarm; i++)
        tick("run", 1);
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static void t_two_fans(void)
{
    printf("A. two fans, the default: the silent intake 2 is judged\n");
    settle();
    armed_session(40);
    CHECK(intake_fans == 2, "the default is two intake fans");
    CHECK(airflow_alarm, "the silent tach trips the airflow gate");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: intake_2 0 under") != NULL, "the fault names intake_2");
    CHECK(strstr(pub_reason, "hold, no resume this job") != NULL, "the hold stands");
    CHECK(!fans_up && !log_has("the armed window is acknowledged"),
          "the armed window was never acknowledged");
    CHECK(strstr(pub_fan_gates, "\"intake_2\":{\"reading\":0,\"floor\":2290,\"state\":\"TRIPPED\"}") != NULL,
          "intake_2 is published tripped at the intake floor");
}

static void t_one_fan(void)
{
    printf("B. one fan: intake 2 is not judged, intake 1 is\n");
    settle();
    set_intake_fans = "1";
    armed_session(40);
    CHECK(intake_fans == 1, "the setting is in force");
    CHECK(log_has("one intake fan fitted (cool_intake_fans 1): intake_2 is not judged"),
          "the session start names the one intake fan");
    CHECK(!airflow_alarm, "no airflow fault through the grace and after");
    CHECK(fans_up && log_has("the armed window is acknowledged"), "the armed window is acknowledged");
    printf("    fan gates: %s\n", pub_fan_gates);
    CHECK(strstr(pub_fan_gates, "\"intake_2\":{\"reading\":0,\"floor\":0,\"state\":\"not fitted\"}") != NULL,
          "intake_2 is published not fitted, with no floor");
    CHECK(strstr(pub_fan_gates, "\"intake_1\":{\"reading\":4167,\"floor\":2290,\"state\":\"ok\"}") != NULL,
          "intake_1 is judged at the intake floor");
}

static void t_one_fan_intake1_silent(void)
{
    printf("C. one fan, and intake 1 stops: intake 1 still trips\n");
    settle();
    set_intake_fans = "1";
    put_long("thermal/tach_intake_1", TACH_NONE);
    armed_session(40);
    CHECK(airflow_alarm, "the airflow gate trips");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: intake_1 0 under the 2290 floor") != NULL,
          "the fault names intake_1 at the intake floor");
    CHECK(!fans_up, "the armed window was never acknowledged");
}

static void t_one_fan_gate_off(void)
{
    printf("D. one fan, the intake gate off: nothing names intake_2\n");
    settle();
    set_intake_fans = "1";
    set_intake_min = "0";
    armed_session(40);
    CHECK(!airflow_alarm, "no airflow fault");
    CHECK(!log_has("gate intake_2 is OFF"), "no would-have-tripped warning names intake_2");
    CHECK(strstr(pub_fan_gates, "\"intake_2\":{\"reading\":0,\"floor\":0,\"state\":\"not fitted\"}") != NULL,
          "intake_2 is still published not fitted");
}

static void t_other_value(void)
{
    printf("E. a hand-edited 0: intake 2 stays judged\n");
    settle();
    set_intake_fans = "0";
    armed_session(40);
    CHECK(airflow_alarm && strstr(pub_reason, "AIRFLOW: intake_2") != NULL,
          "only 1 takes intake_2 out of the judging");
}

int main(void)
{
    make_tree();
    cool_state_model(0);
    t_two_fans();
    t_one_fan();
    t_one_fan_intake1_silent();
    t_one_fan_gate_off();
    t_other_value();
    printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
