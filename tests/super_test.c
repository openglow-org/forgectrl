/*
 * super_test.c - host unit test for the controller-mode supervisor
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The lifecycle thread's one pass, driven by hand against a fake
 * sysfs tree, a fake clock, and fakes of the process calls (fork,
 * waitpid, kill, system, flock) and of the probe and the enclosure
 * read. Every case is a rule the supervisor keeps:
 *
 *   A. a controller death is a signal: SIGCHLD ends the wait at once,
 *      and the pass that follows safes the machine and tells the engine
 *   B. a GRBL controller's homing runner is ended before the respawn,
 *      SIGTERM first, SIGKILL when it stays
 *   C. the respawn waits for the kernel to go idle, bounded: past the
 *      bound the kernel is halted and said so
 *   D. the enclosure check runs before every spawn: an open lid holds a
 *      respawn until it closes, with no second motion probe
 *   E. a busy kernel makes the probe wait and ask again; a missing
 *      accelerometer skips it and the controller starts unverified
 *   F. the stop lever reaches a probe in flight: the ladder ends
 *      between its steps with motion stopped, and the lever returns
 *      once the probe is gone
 *   G. a safing write is tried three times before it is named;
 *      the stop path repeats the pair after the signal
 *   H. a broker fd that cannot be locked refuses the spawn, and an
 *      open that fails keeps trying instead of a self-opening controller
 *   I. the engine's fail tier stops the controller and starts it again
 *      without a backoff
 */
#define _GNU_SOURCE
#define GF_SYSFS  "super-test/sys/"
#define PULSE_DEV "super-test/dev/glowforge"
#define clock_gettime fake_clock_gettime
#define usleep fake_usleep
#define sleep fake_sleep
#define fork fake_fork
#define waitpid fake_waitpid
#define kill fake_kill
#define system fake_system
#define flock fake_flock
#define execle fake_execle
#define liveness_probe fake_liveness_probe
#define liveness_enclosure fake_liveness_enclosure
#define machine_is_idle fake_machine_is_idle

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

int fake_clock_gettime(clockid_t id, struct timespec *ts);
int fake_usleep(unsigned us);
unsigned fake_sleep(unsigned s);
pid_t fake_fork(void);
pid_t fake_waitpid(pid_t pid, int *status, int flags);
int fake_kill(pid_t pid, int sig);
int fake_system(const char *cmd);
int fake_flock(int fd, int op);
int fake_execle(const char *path, const char *arg, ...);
int fake_liveness_probe(int fd, char *detail, size_t dlen);
int fake_liveness_enclosure(char *why, size_t wlen);
int fake_machine_is_idle(void);

#include "../src/super.c"

#include <sys/stat.h>

/* The real clock, for the wait the SIGCHLD case measures. */
#undef clock_gettime
extern int clock_gettime(clockid_t clk, struct timespec *ts);

/* --- the fake clock and sleeps ---------------------------------------- */

static double fake_now = 1000.0;
static const char *create_on_sleep;     /* an attribute a sleep creates (G) */
static int abort_on_sleep;              /* the lever lands this many sleeps on (F) */
static unsigned real_sleep_us;          /* a real pause per fake sleep (F) */

int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    return 0;
}

static void put(const char *attr, const char *v);

int fake_usleep(unsigned us)
{
    fake_now += us / 1e6;
    if (create_on_sleep) {
        put(create_on_sleep, "0\n");
        create_on_sleep = NULL;
    }
    if (abort_on_sleep && --abort_on_sleep == 0)
        probe_abort = 1;
    if (real_sleep_us) {
        struct timespec ts = { 0, (long)real_sleep_us * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

unsigned fake_sleep(unsigned s)
{
    fake_now += s;
    return 0;
}

/* --- the fake processes ----------------------------------------------- */

static pid_t next_pid = 4000;
static pid_t death_pid;                 /* waitpid finds this one dead... */
static int death_status;                /* ...with this status */
static int forks;                       /* controller spawns (relay forks excluded) */
static int kills[64];                   /* the signals sent, in order */
static int nkills;
static int runner_alive;                /* pgrep -x gfhome.py answers 0 */
static int runner_term_ignored;         /* ...and SIGTERM does not end it */
static int flock_fails;
static char calls[4096];                /* the shell commands and forks, in order */

static void note(const char *what)
{
    size_t n = strlen(calls);
    snprintf(calls + n, sizeof(calls) - n, "%s;", what);
}

pid_t fake_fork(void)
{
    static int relay = 1;
    pid_t pid = ++next_pid;
    /* The output relay forks first (and is waited for at once), then
     * the controller. */
    if (relay)
        note("fork-relay");
    else {
        forks++;
        note("fork-controller");
    }
    relay = !relay;
    return pid;
}

pid_t fake_waitpid(pid_t pid, int *status, int flags)
{
    if (flags == 0) {
        *status = 0;
        return pid;             /* the relay's intermediate child */
    }
    if (pid == death_pid) {
        *status = death_status;
        death_pid = 0;
        return pid;
    }
    return 0;
}

int fake_kill(pid_t pid, int sig)
{
    if (nkills < 64)
        kills[nkills] = sig;
    nkills++;
    /* SIGTERM to the child: it dies, the next waitpid finds it. */
    if (sig == SIGTERM && pid > 0)
        death_pid = pid;
    return 0;
}

int fake_system(const char *cmd)
{
    note(cmd);
    if (strstr(cmd, "pgrep -x gfhome.py"))
        return runner_alive ? 0 : 1;
    if (strstr(cmd, "pkill -TERM -x gfhome.py")) {
        if (!runner_term_ignored)
            runner_alive = 0;
        return 0;
    }
    if (strstr(cmd, "pkill -KILL -x gfhome.py")) {
        runner_alive = 0;
        return 0;
    }
    return 1;
}

int fake_flock(int fd, int op)
{
    (void)fd;
    (void)op;
    return flock_fails ? -1 : 0;
}

int fake_execle(const char *path, const char *arg, ...)
{
    (void)path;
    (void)arg;
    return -1;
}

/* --- the fake machine ------------------------------------------------- */

static int probe_rc = 1;                /* what the probe answers */
static int probe_rcs[8], nprobe_rcs, probes;    /* ...or a sequence of answers */
static int enclosure_open;
static int machine_idle = 1;

int fake_liveness_probe(int fd, char *detail, size_t dlen)
{
    (void)fd;
    int rc = probes < nprobe_rcs ? probe_rcs[probes] : probe_rc;
    probes++;
    snprintf(detail, dlen, "fake probe %d", rc);
    return rc;
}

int fake_liveness_enclosure(char *why, size_t wlen)
{
    snprintf(why, wlen, "%s", enclosure_open ? "the lid is open" : "");
    return enclosure_open;
}

int fake_machine_is_idle(void)
{
    return machine_idle;
}

/* --- the stubs the supervisor links against ---------------------------- */

static char last_log[256], logs[8192];
static int crit_logs;

void fflog(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (prio == LOG_CRIT)
        crit_logs++;
    size_t n = strlen(logs);
    snprintf(logs + n, sizeof(logs) - n, "%s\n", last_log);
    printf("    log: %s\n", last_log);
}

static int engine_stops;
void cool_controller_stopped(void) { engine_stops++; }
double cool_report_age(void) { return -1.0; }
void led_set(led_pattern_t p) { (void)p; }
void led_release(void) { }
void lenshome_clear(void) { }
int lenshome_run(char *detail, size_t dlen) { snprintf(detail, dlen, "fake edge"); return 1; }
void cam_lamp_apply_idle(void) { }
int setup_gate_open(char *why, size_t len) { if (len) why[0] = '\0'; return 1; }
int diag_running(void) { return 0; }
int update_job_running(void) { return 0; }
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }
int settings_get_bool(const char *key, int def) { (void)key; return def; }
int settings_set(const char *key, const char *val) { (void)key; (void)val; return 0; }

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
    mkdir("super-test", 0755);
    mkdir("super-test/dev", 0755);
    mkdir("super-test/data", 0755);
    mkdir(GF_SYSFS, 0755);
    mkdir(GF_SYSFS "cnc", 0755);
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    put("cnc/halt", "0\n");
    put("cnc/enable", "0\n");
    put("cnc/disable", "0\n");
    FILE *f = fopen(PULSE_DEV, "w");
    if (f)
        fclose(f);
    setenv("FORGECTRL_DATA_DIR", "super-test/data", 1);
}

static void clear_safing(void)
{
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    put("cnc/halt", "0\n");
}

static int safed(void)
{
    return get_long("cnc/stop") == 1 && get_long("cnc/laser_latch") == 1;
}

/* --- driving the supervisor ------------------------------------------- */

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* One pass of the lifecycle, the way the thread runs it. */
static void tick(void)
{
    pthread_mutex_lock(&mu);
    pass_locked();
    pthread_mutex_unlock(&mu);
}

/* Passes until a controller runs, or the fake clock has moved `s`. */
static int run_until_spawned(double s)
{
    double until = fake_now + s;
    while (fake_now < until) {
        tick();
        if (getenv("DEBUG"))
            printf("    t=%.1f until=%.1f child=%d probed=%d respawn_at=%.1f wait_idle=%d bfd=%d\n",
                   fake_now, until, (int)child_pid, probed, respawn_at, respawn_wait_idle, broker_fd);
        if (child_pid > 0)
            return 1;
        fake_now += 0.2;
    }
    return 0;
}

/* Back to a supervised GRBL mode with nothing running and the machine
   at rest: the state a case starts from. */
static void reset_state(void)
{
    pthread_mutex_lock(&mu);
    want = Ctl_Grbl;
    suspended = 0;
    child_pid = 0;
    child_ctl = Ctl_None;
    backoff_s = RESPAWN_MIN_S;
    respawn_at = 0.0;
    motion_fault = 0;
    probed = 0;
    probe_skipped = 0;
    probing = 0;
    probe_abort = 0;
    enclosure_wait = 0;
    wait_why[0] = '\0';
    runner_check = 0;
    respawn_wait_idle = 0;
    restart_pending = 0;
    standby_takeover = 0;
    gated = 0;
    if (broker_fd >= 0) {
        close(broker_fd);
        broker_fd = -1;
    }
    broker_warned = 0;
    pthread_mutex_unlock(&mu);
    death_pid = 0;
    nkills = 0;
    forks = 0;
    probes = 0;
    nprobe_rcs = 0;
    probe_rc = 1;
    enclosure_open = 0;
    machine_idle = 1;
    runner_alive = 0;
    runner_term_ignored = 0;
    flock_fails = 0;
    engine_stops = 0;
    crit_logs = 0;
    calls[0] = '\0';
    logs[0] = '\0';
    clear_safing();
}

/* A controller up and verified: the broker held, the probe passed, the
   child spawned. */
static void bring_up(void)
{
    reset_state();
    CHECK(run_until_spawned(5.0), "a controller comes up");
    CHECK(probed && !probe_skipped && probes == 1, "behind one passed motion probe");
    clear_safing();
    calls[0] = '\0';
}

/* The running controller dies with this status; the next pass reaps. */
static void die(int status)
{
    death_pid = child_pid;
    death_status = status;
}

static double now_real(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void *stop_thread(void *arg)
{
    *(int *)arg = super_controller_stop();
    return NULL;
}

int main(void)
{
    make_tree();

    printf("A. a controller death is a signal\n");
    bring_up();
    super_sigchld_init();
    CHECK(chld_efd >= 0, "the SIGCHLD eventfd is open");
    double t0 = now_real();
    raise(SIGCHLD);
    loop_wait(2000);
    double waited = now_real() - t0;
    CHECK(waited < 0.5, "SIGCHLD ends a 2 s wait at once");
    t0 = now_real();
    loop_wait(100);
    CHECK(now_real() - t0 >= 0.09, "with nothing pending the wait is the tick");
    die(0x0900);
    tick();
    CHECK(child_pid == 0, "the pass reaps the dead controller");
    CHECK(safed(), "and safes the machine: motion stopped, the latch locked");
    CHECK(engine_stops == 1, "and tells the engine its reporter is gone");
    CHECK(strstr(last_log, "respawn in") != NULL, "the respawn is armed");

    printf("B. the homing runner is ended before the respawn\n");
    bring_up();
    runner_alive = 1;
    die(0x0900);
    tick();
    CHECK(strstr(calls, "pkill -TERM -x gfhome.py") != NULL, "a GRBL death looks for the runner and ends it");
    CHECK(!runner_alive, "the runner is gone");
    CHECK(strstr(calls, "fork-controller") == NULL, "no controller was started while it lived");
    CHECK(run_until_spawned(5.0), "the respawn follows");
    CHECK(strstr(calls, "pkill -TERM") < strstr(calls, "fork-controller"), "the runner's end came first");
    bring_up();
    runner_alive = 1;
    runner_term_ignored = 1;
    double t_dead = fake_now;
    die(0x0900);
    tick();
    CHECK(strstr(calls, "pkill -KILL -x gfhome.py") != NULL && !runner_alive,
          "a runner that ignores SIGTERM is killed");
    CHECK(fake_now - t_dead >= RUNNER_TERM_WAIT_S, "after its SIGTERM grace");
    CHECK(run_until_spawned(5.0) && probes == 1, "the respawn follows, with no second motion probe");

    printf("C. the respawn waits for the kernel, bounded\n");
    bring_up();
    machine_idle = 0;
    die(0x0900);
    tick();
    fake_now += 2.0;
    CHECK(!run_until_spawned(5.0), "no respawn while the kernel runs (7 s)");
    machine_idle = 1;
    CHECK(run_until_spawned(1.0), "the respawn follows the kernel's idle");
    CHECK(get_long("cnc/halt") == 0, "nothing was halted");
    bring_up();
    machine_idle = 0;
    die(0x0900);
    tick();
    fake_now += 2.0;
    CHECK(run_until_spawned(12.0), "past the bound the respawn goes ahead");
    CHECK(get_long("cnc/halt") == 1, "with the kernel halted first");
    CHECK(strstr(logs, "still running") != NULL && crit_logs > 0, "and said at the highest severity");
    machine_idle = 1;

    printf("D. the enclosure check runs before every spawn\n");
    bring_up();
    die(0x0900);
    tick();
    enclosure_open = 1;
    fake_now += 2.0;
    CHECK(!run_until_spawned(5.0), "an open lid holds the respawn");
    CHECK(enclosure_wait && !strcmp(wait_why, "the lid is open"), "and the supervisor says what is open");
    enclosure_open = 0;
    CHECK(run_until_spawned(1.0), "the respawn follows the lid");
    CHECK(!enclosure_wait, "the wait is over");
    CHECK(probes == 1, "the motion probe ran once for the broker hold, not again");

    printf("E. a busy kernel makes the probe wait; a missing accelerometer skips it\n");
    reset_state();
    probe_rcs[0] = -3;
    probe_rcs[1] = -3;
    probe_rcs[2] = 1;
    nprobe_rcs = 3;
    CHECK(run_until_spawned(10.0), "a controller comes up");
    CHECK(probes == 3 && probed && !probe_skipped, "after two waits the probe passed");
    CHECK(strstr(logs, "WAITS") != NULL, "the waits were said as waits, not errors");
    reset_state();
    probe_rc = -1;
    CHECK(run_until_spawned(5.0), "a controller comes up");
    CHECK(probes == 1 && probed && probe_skipped, "the probe skipped once: unverified");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "\"motion\":\"unverified\"") != NULL, "and /mode says unverified");
    }

    printf("F. the stop lever reaches a probe in flight\n");
    reset_state();
    probe_rc = 0;                       /* NO MOTION: the ladder waits 5 s */
    abort_on_sleep = 12;                /* the lever lands in that wait, after the first probe */
    pthread_mutex_lock(&mu);
    probing = 1;
    pthread_mutex_unlock(&mu);
    int rc = probe_sequence(3);
    pthread_mutex_lock(&mu);
    probing = 0;
    pthread_mutex_unlock(&mu);
    CHECK(rc == 4, "the sequence ends with the lever's code");
    CHECK(get_long("cnc/stop") == 1, "with motion stopped");
    CHECK(probes == 1, "before the second rung probed");
    CHECK(strstr(probe_detail, "stop lever") != NULL, "and says why");
    clear_safing();
    reset_state();
    pthread_mutex_lock(&mu);
    probing = 1;
    pthread_mutex_unlock(&mu);
    real_sleep_us = 1000;
    int stop_rc = -2;
    pthread_t th_stop;
    pthread_create(&th_stop, NULL, stop_thread, &stop_rc);
    {
        struct timespec ts = { 0, 30000000 };
        nanosleep(&ts, NULL);           /* the lever is waiting on the probe */
    }
    CHECK(probe_abort == 1 && get_long("cnc/stop") == 1, "the lever asked the probe to end and stopped motion");
    pthread_mutex_lock(&mu);
    probing = 0;                        /* the probe returns */
    pthread_mutex_unlock(&mu);
    pthread_join(th_stop, NULL);
    real_sleep_us = 0;
    CHECK(stop_rc == 0, "the lever returns once the probe is gone");
    CHECK(suspended, "supervision stays suspended");

    printf("G. a safing write is tried three times; the stop repeats the pair\n");
    reset_state();
    unlink(GF_SYSFS "cnc/stop");
    create_on_sleep = "cnc/stop";       /* the attribute appears after the first retry wait */
    CHECK(wr_attr("cnc/stop", "1") == 0 && get_long("cnc/stop") == 1,
          "a write that fails once lands on its retry");
    unlink(GF_SYSFS "cnc/stop");
    crit_logs = 0;
    CHECK(wr_attr("cnc/stop", "1") == -1 && crit_logs == 1 && strstr(last_log, "3 times") != NULL,
          "three failures are named once, at the highest severity");
    put("cnc/stop", "0\n");
    bring_up();
    unlink(GF_SYSFS "cnc/laser_latch");
    pthread_mutex_lock(&mu);
    suspended = 1;
    stop_locked();
    pthread_mutex_unlock(&mu);
    CHECK(child_pid == 0 && nkills >= 2 && kills[0] == SIGTERM, "the stop signals and reaps the controller");
    CHECK(strstr(logs, "laser_latch failed") != NULL, "a latch write that keeps failing is named");
    put("cnc/laser_latch", "0\n");

    printf("H. the broker fails closed\n");
    reset_state();
    flock_fails = 1;
    CHECK(!run_until_spawned(3.0), "a broker fd that cannot be locked starts no controller");
    CHECK(broker_fd < 0 && strstr(logs, "flock") != NULL && crit_logs == 1,
          "the fd is let go and the refusal said once");
    flock_fails = 0;
    CHECK(run_until_spawned(3.0), "the next try holds the broker and the controller comes up");
    reset_state();
    unlink(PULSE_DEV);
    CHECK(!run_until_spawned(4.0), "a pulse device that cannot be opened starts no controller");
    CHECK(strstr(logs, "cannot open") != NULL && crit_logs == 1, "the refusal is said once while it lasts");
    {
        FILE *f = fopen(PULSE_DEV, "w");
        if (f)
            fclose(f);
    }
    CHECK(run_until_spawned(4.0), "the controller comes up once the device opens");

    printf("I. the engine's fail tier stops the controller and starts it again\n");
    bring_up();
    pid_t before = child_pid;
    super_controller_restart("head crash signal");
    CHECK(restart_pending, "the request is posted");
    tick();
    CHECK(child_pid == 0 || child_pid != before, "the controller was stopped");
    CHECK(nkills >= 1 && kills[0] == SIGTERM, "the deliberate way: SIGTERM after the safing pair");
    CHECK(safed(), "motion stopped and the latch locked");
    CHECK(engine_stops >= 1, "the engine forgets the reporter");
    CHECK(run_until_spawned(1.0) && child_pid != before, "a new controller follows without a backoff");
    CHECK(probes == 1, "and without a second motion probe");

    printf(failures ? "super_test: %d FAILED\n" : "super_test: all ok\n", failures);
    return failures ? 1 : 0;
}
