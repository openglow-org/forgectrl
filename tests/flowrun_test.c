/*
 * flowrun_test.c - host unit test for the coolant flow watch's heater writer
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * flowrun.c on a fake clock against a fake sysfs tree: the thread's step
 * driven by hand, the engine's context handed in as the engine would. Each
 * case is a rule of the writer, the part flowwatch_test cannot see:
 *
 *   A. cool_flow_watch unset: the watch is off, the heater never written and
 *      the sampler's file never read
 *   B. turned on with no readings: the heater off, the missing sampler named
 *      once; then readings, a rest, and the hold heats; a 1 s window short
 *      of half the sampler's rate is no reading
 *   C. the kernel's lease: a heater above zero is written again every second
 *   D. the engine takes the heater: off at once and unstamped, never
 *      overwritten while the engine holds it, and back only after a rest; a
 *      tick that began before the yield lets nothing go
 *   E. an engine that stops ticking: after 3 s the heater off, the watch
 *      paused and saying why; back after a rest once it ticks again
 *   F. the readings corrected as the engine corrects its own: the 45 C rail
 *      on the corrected downstream reading, a half degree either side of it
 *   G. a start: with no heater-off stamp (a daemon killed in a hold) the
 *      full rest; with a stamp a minute old or more, the short one
 *   H. cool_flow_watch 0: the heater off within the 5 s setting poll
 *   I. the three numbers read from the settings store at the poll: a number
 *      taken, one that is not a number at its default, one past its range
 *      held to it
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* A sysfs write replaces the value; a plain file in the fake tree needs
 * truncating for that. Every open of the sampler's file is counted. */
static int means_opens;
static int fake_open(const char *path, int flags, ...)
{
    if (strstr(path, "pic/means"))
        means_opens++;
    return openat(AT_FDCWD, path, (flags & O_WRONLY) ? (flags | O_TRUNC) : flags, 0644);
}
#define open fake_open
#define clock_gettime fake_clock_gettime
#define pthread_create fake_pthread_create
#define pthread_join fake_pthread_join
#include "../src/flowrun.c"

#include <stdarg.h>
#include <sys/stat.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* --- fakes ------------------------------------------------------------ */

static double fake_now = 5000.0;
int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)ts->tv_sec) * 1e9);
    return 0;
}

int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    (void)t; (void)a; (void)fn; (void)arg;
    return 0;                       /* the test steps the loop itself */
}
int fake_pthread_join(pthread_t t, void **ret)
{
    (void)t; (void)ret;
    return 0;
}

static char logbuf[16384];
void fflog(int prio, const char *fmt, ...)
{
    (void)prio;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof(logbuf))
        snprintf(logbuf + n, sizeof(logbuf) - n, "%s\n", line);
    if (getenv("VERBOSE"))
        printf("    log: %s\n", line);
}
static int log_count(const char *needle)
{
    int n = 0;
    for (const char *p = logbuf; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

/* The stored settings: NULL is a key that is not set, and the caller's
 * default answers for it. */
static const char *watch_setting;
static const char *num_kv[8];           /* key, value, ..., NULL: the three numbers */
int settings_get(const char *key, char *val, size_t len)
{
    for (size_t i = 0; num_kv[i]; i += 2)
        if (!strcmp(num_kv[i], key)) {
            snprintf(val, len, "%s", num_kv[i + 1]);
            return 0;
        }
    return -1;
}
int settings_get_bool(const char *key, int def)
{
    if (!strcmp(key, "cool_flow_watch") && watch_setting)
        return watch_setting[0] == '1';
    return def;
}

/* The factory conversion (status.c). */
double coolant_degc_counts(double raw)
{
    static const double adc_f = 1024.0 * 1.3;
    if (raw <= 0.0 || raw >= adc_f)
        return -273.15;
    double r = 10000.0 / (adc_f / raw - 1.0);
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    return 3380.0 / log(r / rinf) - 273.15;
}

/* --- the tree --------------------------------------------------------- */

#define ROOT "flowrun-test/"

static void put(const char *attr, const char *v)
{
    char path[160];
    snprintf(path, sizeof(path), ROOT "sys/%s", attr);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(v, f);
        fclose(f);
    }
}

static long heater(void)
{
    char buf[24];
    FILE *f = fopen(ROOT "sys/thermal/heater_pwm", "r");
    if (!f)
        return -1;
    long v = fgets(buf, sizeof(buf), f) ? strtol(buf, NULL, 10) : -1;
    fclose(f);
    return v;
}

/* pic/means as the kernel prints it (pic_sampler_means_show): the rate
 * first, then a line a channel, ten of them: the 1 s mean, the 10 s mean,
 * the 1 s window's least and most, the reads in the 1 s and the 10 s
 * windows. @down and @up are the two coolant channels' 1 s means in
 * counts, @in_1s the reads in every channel's 1 s window. */
static void means_put(int down, int up, int in_1s)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "hz 50 age_ms 12 reads 1000 errors 0\n"
             "water_temp_1 %d.000 %d.000 %d %d %d 500\n"
             "water_temp_2 %d.000 %d.000 %d %d %d 500\n"
             "tec_temp 1023.000 1023.000 1023 1023 %d 500\n"
             "pwr_temp 559.120 559.080 558 560 %d 500\n"
             "lid_ir_1 50.000 50.100 49 51 %d 500\n"
             "lid_ir_2 49.000 49.000 48 50 %d 500\n"
             "lid_ir_3 52.000 52.000 51 53 %d 500\n"
             "lid_ir_4 54.000 54.000 53 55 %d 500\n"
             "hv_current 0.000 0.000 0 0 %d 500\n"
             "hv_voltage 0.000 0.000 0 0 %d 500\n",
             down, down, down - 1, down + 1, in_1s, up, up, up - 1, up + 1, in_1s,
             in_1s, in_1s, in_1s, in_1s, in_1s, in_1s, in_1s, in_1s);
    put("pic/means", text);
}

/* The same reading on both channels, so the hold has its step to make; or
 * no file at all (a kernel module with no sampler). */
static void means_file(int on)
{
    if (on)
        means_put(730, 730, 50);
    else
        unlink(ROOT "sys/pic/means");
}

/* --- driving ---------------------------------------------------------- */

static int engine_ticking = 1;
static flowrun_ctx_t eng;
static double next_tick;

static void step(double seconds)
{
    double until = fake_now + seconds;
    while (fake_now < until) {
        fake_now += 0.05;
        if (engine_ticking && fake_now >= next_tick) {
            next_tick = fake_now + 1.0;
            eng.t = fake_now;
            flowrun_engine(&eng);
        }
        run_step(fake_now);
    }
}

int main(void)
{
    setenv("FORGECTRL_HOST_TEST", "1", 1);
    setenv("GF_SYSFS_ROOT", ROOT "sys/", 1);
    setenv("GF_RUN_DIR", ROOT "run", 1);
    mkdir(ROOT, 0755);
    mkdir(ROOT "sys", 0755);
    mkdir(ROOT "sys/pic", 0755);
    mkdir(ROOT "sys/thermal", 0755);
    mkdir(ROOT "run", 0755);
    unlink(ROOT "run/" STAMP_FILE);
    memset(&eng, 0, sizeof(eng));
    eng.fans = FW_FANS_IDLE;

    printf("A. cool_flow_watch unset: off\n");
    /* Readings there, and a value in the heater's file that no write of
     * this file's gives: the time of a full rest and a hold's start on. */
    means_file(1);
    put("thermal/heater_pwm", "7\n");
    flowrun_init();
    step(75.0);
    CHECK(fw.mode == FW_OFF && heater() == 7, "unset is off: 75 s on, the heater never written");
    CHECK(means_opens == 0, "and the sampler's file never read");
    put("thermal/heater_pwm", "0\n");

    printf("B. turned on: no readings, then readings: the rest, the hold\n");
    watch_setting = "1";
    means_file(0);
    step(70.0);
    CHECK(heater() == 0 && fw.mode == FW_REST, "no readings: resting, the heater off");
    CHECK(log_count("sampler gives no readings (pic/means)") == 1, "the missing sampler named once");
    CHECK(strstr(status_buf, "sampler gives no readings") != NULL, "and /cool/flow says why");
    {
        double d, u;
        means_put(730, 730, 24);
        CHECK(!means(&d, &u, 0.0, 0.0), "24 reads in the 1 s window at 50 Hz: no reading");
        means_put(730, 730, 25);
        CHECK(means(&d, &u, 0.0, 0.0) && fabs(d - coolant_degc_counts(730.0)) < 1e-9,
              "25 reads: the window's mean is the reading");
    }
    means_file(1);
    step(12.0);
    CHECK(fw.mode == FW_HOLD && heater() > 0, "readings: the hold heats");
    CHECK(access(ROOT "run/" STAMP_FILE, F_OK) != 0, "and no heater-off stamp stands while it does");

    printf("C. the kernel's lease\n");
    /* Downstream far under the step, so the hold sits at its most and its
     * duty stands still: only the refresh writes the heater again (a duty
     * that moves would be written anyway). Well inside the 20 s that would
     * halt a pinned hold. */
    means_put(900, 730, 50);
    step(3.0);
    long pinned = heater();
    put("thermal/heater_pwm", "0\n");   /* the kernel lapsed it */
    step(1.1);
    CHECK(pinned == lround(FLOWCALC_DUTY_MAX_DEF * 65535.0 / 100.0) && heater() == pinned,
          "a heater above zero is written again within a second, its duty unchanged");
    means_file(1);
    step(5.0);

    printf("D. the engine takes the heater\n");
    flowrun_yield("warm-up");
    CHECK(heater() == 0, "off at once");
    CHECK(access(ROOT "run/" STAMP_FILE, F_OK) != 0, "and unstamped: what the engine does with the heater is not off");
    eng.yield = 1;
    eng.yield_why = "warm-up";
    put("thermal/heater_pwm", "26214\n");   /* the engine's warm-up */
    step(5.0);
    CHECK(heater() == 26214 && fw.mode == FW_SUSPENDED, "never written while the engine holds it");
    eng.yield = 0;
    eng.yield_why = NULL;
    put("thermal/heater_pwm", "0\n");
    step(30.0);
    CHECK(heater() == 0 && fw.mode == FW_REST, "the engine lets go: a rest first");
    step(35.0);
    CHECK(fw.mode == FW_HOLD && heater() > 0, "then the hold");
    /* A tick that began before the yield hands in its context after it:
     * it saw no cause, and must not let go of the heater the yield took. */
    flowrun_yield("a quiet hold");
    eng.yield = 0;
    eng.t = fake_now - 0.5;
    flowrun_engine(&eng);
    engine_ticking = 0;
    step(2.0);
    CHECK(heater() == 0 && fw.mode == FW_SUSPENDED, "a tick from before the yield lets nothing go");
    engine_ticking = 1;
    next_tick = 0.0;
    step(1.0);
    CHECK(fw.mode == FW_REST, "a tick from after it lets go: a rest");
    step(64.95);
    next_tick = 0.0;                    /* the engine's last tick as E begins */
    step(0.05);

    printf("E. an engine that stops ticking\n");
    engine_ticking = 0;
    step(2.5);
    CHECK(heater() > 0, "under 3 s: the hold goes on");
    step(1.5);
    CHECK(heater() == 0 && fw.mode == FW_SUSPENDED, "past 3 s: the heater off, the watch paused");
    CHECK(strstr(status_buf, "the cooling engine stopped ticking") != NULL, "and /cool/flow says why");
    put("thermal/heater_pwm", "0\n");
    step(10.0);
    CHECK(heater() == 0, "nothing written while it stays stopped");
    engine_ticking = 1;
    next_tick = 0.0;
    step(70.0);
    CHECK(fw.mode == FW_HOLD && heater() > 0, "ticking again: back after a rest");

    printf("F. the readings corrected as the engine's\n");
    CHECK(fabs(corrected_c(730.0, 12.0, 0.5) - (coolant_degc_counts(718.0) + 0.5)) < 1e-9,
          "the air-assist shift off, the per-machine correction on");
    /* The rail by its number: the per-machine correction puts the
     * corrected downstream a half degree either side of 45 C. */
    double read_c = coolant_degc_counts(730.0);
    eng.temp_offset_c = 45.5 - read_c;
    step(2.0);
    CHECK(heater() == 0 && fw.mode == FW_HOLD, "the corrected downstream at 45.5 C: the heater off");
    eng.temp_offset_c = 44.5 - read_c;
    step(3.0);
    CHECK(heater() > 0, "at 44.5 C: the hold heats");
    eng.temp_offset_c = 0.0;
    step(3.0);

    printf("G. a start, with and without the heater-off stamp\n");
    /* The daemon killed in the hold: its successor finds no stamp, and the
     * kernel's lease has lapsed the heater. */
    CHECK(heater() > 0 && access(ROOT "run/" STAMP_FILE, F_OK) != 0, "a hold heats, and no stamp stands");
    written = 0.0;
    put("thermal/heater_pwm", "0\n");
    flowrun_init();
    step(12.0);
    CHECK(fw.mode == FW_REST && heater() == 0, "no stamp: the heater taken as just off, 12 s on still resting");
    step(50.0);
    CHECK(fw.mode == FW_HOLD && heater() > 0, "the hold after the full rest");
    /* A stop writes the stamp; a start 100 s after it rests the short time. */
    flowrun_shutdown();
    CHECK(heater() == 0 && access(ROOT "run/" STAMP_FILE, F_OK) == 0, "a stop: the heater off, stamped");
    fake_now += 100.0;
    flowrun_init();
    step(12.0);
    CHECK(fw.mode == FW_HOLD && heater() > 0, "a stamp 100 s old: the hold after the 10 s rest");

    printf("H. cool_flow_watch 0\n");
    watch_setting = "0";
    step(5.1);
    CHECK(heater() == 0 && fw.mode == FW_OFF, "off within the 5 s setting poll");

    printf("I. the three numbers from the settings store\n");
    num_kv[0] = "cool_flow_hold_c"; num_kv[1] = "abc";
    num_kv[2] = "cool_flow_duty_min"; num_kv[3] = "1.5";
    num_kv[4] = "cool_flow_duty_max"; num_kv[5] = "30"; num_kv[6] = NULL;
    step(5.1);
    CHECK(cfg.hold_c == FLOWCALC_HOLD_C_DEF && cfg.duty_min == 1.5 && cfg.duty_max == FLOWCALC_DUTY_MAX_HI,
          "at the next poll: a stored number taken, one that is not a number at its default, one past its range held");
    num_kv[0] = NULL;
    step(5.1);

    flowrun_shutdown();
    printf(failures ? "flowrun_test: %d FAILED\n" : "flowrun_test: all ok\n", failures);
    return failures ? 1 : 0;
}
