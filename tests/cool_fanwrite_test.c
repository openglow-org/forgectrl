/*
 * cool_fanwrite_test.c - host unit test for the verified fan writes
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Every fan duty the engine writes is read back, tried again when the
 * device did not take it, and held to the command once a tick. The
 * bench found why: a run session opened, the engine wrote the air
 * assist's run duty, the head kept its idle duty, and the engine, with
 * nothing to tell it, judged the fan against its floor and held the
 * job as a slow fan. This test drives the engine tick by tick against a
 * fake sysfs tree whose writes can be dropped or refused:
 *
 *   A. clean:        a session opens and every fan reads its run duty
 *   B. dropped:      two writes dropped, the third takes; no fault
 *   C. lost:         a device loses its duty mid-session; the tick puts
 *                    it back, names it, and the fan gets its spin-up
 *                    grace, so a fan that then runs is not tripped
 *   D. refused:      a device takes nothing; the fan is tripped at its
 *                    tach and the fault names the duty and the readback
 *   E. thermal lost: the same readback route holds the exhaust PWM
 */
#define GF_SYSFS    "cool-fanwrite-test/sys/"
#define VERDICT_DIR "cool-fanwrite-test/run"
#define clock_gettime fake_clock_gettime
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* The fake tree is plain files: a write truncates, as a sysfs store
 * replaces. The one attribute under test can drop writes (the device
 * answers, keeps its value) or refuse them (the bus fails). */
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

static const char *kv[] = { "cool_tach_air_assist_min_rpm", "6093", NULL };

int settings_get(const char *key, char *val, size_t len)
{
    for (size_t i = 0; kv[i]; i += 2)
        if (!strcmp(kv[i], key)) {
            snprintf(val, len, "%s", kv[i + 1]);
            return 0;
        }
    return -1;
}

static char last_log[256];
static char last_warn[256];
static char logbuf[16384];          /* every line since the last settle */
static int warnings;

void fflog(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (prio == LOG_WARNING) {
        warnings++;
        snprintf(last_warn, sizeof(last_warn), "%s", last_log);
    }
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
int machine_is_idle(void) { return 1; }
int commission_flag(const char *id, const char *level, const char *reason)
{
    (void)id; (void)level; (void)reason;
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

/* Tach periods: 300 is a fast fan; 3518 is the air assist at its idle
 * duty on the bench reference (about 2130 rpm, under the 6093 floor). */
#define AA_FAST 300
#define AA_IDLE 3518

static void make_tree(void)
{
    mkdir("cool-fanwrite-test", 0755);
    mkdir("cool-fanwrite-test/run", 0755);
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
    put_long("thermal/tach_intake_1", 300);
    put_long("thermal/tach_intake_2", 300);
    put_long("head/air_assist_tach", AA_FAST);
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

/* Close any session and settle idle, the fake device honest again. */
static void settle(void)
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
    warnings = 0;
    last_warn[0] = '\0';
    logbuf[0] = '\0';
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static void t_clean(void)
{
    printf("A. a session opens and every fan reads its run duty\n");
    settle();
    tick("run", 1);
    CHECK(cool_state == Cool_Run, "the session is open");
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "air assist at run duty");
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN, "exhaust at run duty");
    CHECK(get_long("thermal/intake_pwm") == INTAKE_RUN, "intake at run duty");
    CHECK(fan_cmd[Fx_AirAssist] == AIR_ASSIST_RUN, "the command mirror agrees");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!log_has("did not take") && !log_has("commanded: "),
          "no fan write named through the grace and after");
    CHECK(!airflow_alarm, "no airflow fault");
}

static void t_dropped(void)
{
    printf("B. two writes dropped, the third takes\n");
    settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = 2;
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "the third try took");
    CHECK(log_has("head/air_assist_pwm took 1023 on try 3"), "the retry is named");
    CHECK(!log_has("did not take"), "no failed write named: it got through");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no airflow fault");
}

static void t_lost(void)
{
    printf("C. a device loses its duty mid-session\n");
    settle();
    tick("run", 1);
    for (int i = 0; i < 20; i++)
        tick("run", 1);                 /* past the spin-up grace */
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "at run duty before the loss");
    /* The head resets: its register back at idle, the fan winding down. */
    put_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_RUN, "the tick put the duty back");
    CHECK(log_has("head/air_assist_pwm read 204 with 1023 commanded: put back"),
          "the loss is named");
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

static void t_refused(void)
{
    printf("D. a device takes nothing: the fault names the duty and the readback\n");
    settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = -1;
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    CHECK(get_long("head/air_assist_pwm") == AIR_ASSIST_IDLE, "the head kept its idle duty");
    CHECK(log_has("head/air_assist_pwm did not take 1023: it reads 204 after 3 tries"),
          "the failed write is named at once");
    for (int i = 0; i < 25 && !airflow_alarm; i++)
        tick("run", 1);
    CHECK(airflow_alarm, "the fan is tripped at its tach");
    printf("    reason: %s\n", pub_reason);
    CHECK(strstr(pub_reason, "AIRFLOW: air_assist") != NULL, "the airflow fault");
    CHECK(strstr(pub_reason, "(duty 1023, reads 204)") != NULL,
          "the fault names the duty commanded and the duty in force");
    CHECK(strstr(pub_reason, "hold, no resume this job") != NULL, "the hold stands");
}

static void t_refused_bus(void)
{
    printf("D2. the bus refuses the write: named, and the fault the same\n");
    settle();
    drop_attr = "head/air_assist_pwm";
    drop_left = -1;
    refuse = 1;
    put_long("head/air_assist_tach", AA_IDLE);
    tick("run", 1);
    CHECK(log_has("did not take 1023: it reads 204 after 3 tries"),
          "the refused write is named");
    for (int i = 0; i < 25 && !airflow_alarm; i++)
        tick("run", 1);
    CHECK(airflow_alarm && strstr(pub_reason, "(duty 1023, reads 204)") != NULL,
          "the fault names the duty in force");
}

static void t_thermal_lost(void)
{
    printf("E. the exhaust PWM loses its duty: the same route puts it back\n");
    settle();
    tick("run", 1);
    for (int i = 0; i < 20; i++)
        tick("run", 1);
    put_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    tick("run", 1);
    CHECK(get_long("thermal/exhaust_pwm") == EXHAUST_RUN, "the exhaust duty is put back");
    CHECK(log_has("thermal/exhaust_pwm read 0 with 65535 commanded: put back"),
          "the loss is named");
    CHECK(fan_lost[Fx_Exhaust] == 1 && fan_lost[Fx_AirAssist] == 0, "counted on the right fan");
    for (int i = 0; i < 25; i++)
        tick("run", 1);
    CHECK(!airflow_alarm, "no fault");
}

int main(void)
{
    make_tree();
    cool_state_model(0);
    t_clean();
    t_dropped();
    t_lost();
    t_refused();
    t_refused_bus();
    t_thermal_lost();
    printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
