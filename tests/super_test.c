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
 *  D2. the first spawn of a broker hold waits for it the same way: no
 *      probe and no fork while the lid is open, /mode says waiting and
 *      why, and the button blinks amber; once it closes the probe runs,
 *      then the spawn
 *   E. a busy kernel makes the probe wait and ask again; a missing
 *      accelerometer skips it and the controller starts unverified
 *   F. the stop lever reaches a probe in flight: the ladder ends
 *      between its steps with motion stopped, within its rail-off wait,
 *      and the lever returns once the probe is gone; a lever that cuts a
 *      probe short is followed by no rail-off rung, and one during the
 *      lens hunt ends the hunt with no marker
 *   G. a safing write is tried three times before it is named;
 *      the stop path lands the stop before the signal and repeats the
 *      pair after it
 *   H. a broker fd that cannot be locked refuses the spawn, and an
 *      open that fails keeps trying instead of a self-opening controller;
 *      GF_PULSE_DEV moves the device, and the refusal names the path
 *      it tried; one that is not an absolute path is refused by its
 *      form, the device it would reach left unopened
 *   I. the engine's fail tier stops the controller the deliberate way,
 *      the safing pair landed before the SIGTERM, and starts it again
 *      without a backoff
 *   J. a motor release is nobody's to end but the operator's: the probe
 *      is skipped with nothing written to the rail, the lens is still
 *      swept before the controller and waits for an open lid, the
 *      controller still starts, and a switch to cloud mode is refused
 *   L. the report channel is the running controller's alone: each spawn
 *      is handed a fresh secret in its environment, an inherited one is
 *      replaced, nothing else passes for it, and a reap ends it
 *   M. a dev image starts a controller only when the boot's data record
 *      says where its settings are (emmc, card, card-none): a record
 *      that is missing or holds any other word holds the controllers,
 *      and a release image is not held by the record
 *   N. the button is solid white while the probe moves the machine and out
 *      after it; a probe that finds no motion is the fault the cooling
 *      engine reads (super_motion_fault), no pass after it probes or
 *      starts a controller, and the retry clears it and probes first
 *   O. switches that cannot be read hold the gate as an open lid does; a
 *      probe the enclosure ends, and a lid opened before the lens hunt,
 *      decide nothing and the probe runs again
 *   P. a probe that fails on the machine, and a lens with no edge, are
 *      motion faults in their own words, with no rail cycle
 *   Q. the kernel as the last holder left it: a stepper fault gets one
 *      enable (named, and a motion fault in its own words when refused,
 *      never retried by the passes), an underrun is acknowledged, a
 *      kernel still playing is waited for, bounded, then halted; a
 *      respawn over a stepper fault is a motion fault, not a halt and a
 *      spawn; a shutdown and a standby treat a fault as playing nothing
 *   R. a check's hand-back ends its own takeover alone: an operator's
 *      stop and a motion fault outlive it; a check's probe that ends
 *      before a verdict leaves motion unverified, and one the lever ends
 *      leaves the operator's stop standing; a homing runner is ended
 *      before the check's probe
 *   S. the lens marker goes at every reap and the lens is swept again
 *      before the next GRBL controller; a sweep with no edge starts the
 *      controller with Z unreferenced and is not repeated in the hold
 *      until a hunt finds the edge; the real lever, from its own thread,
 *      ends the sweep between two steps and returns only after it; a
 *      hold after its last step, and switches that stop answering during
 *      it, hold the spawn; the cloud client gets no sweep
 *   T. a lever or a takeover that lands in the pass's enclosure read,
 *      with the lock down, is followed by no lens step and no spawn
 *   U. the lever reaches the wait for a busy kernel; an enable refused at
 *      a later rung is a motion fault; /mode names a stepper fault by what
 *      the kernel holds; a third move stopped under an opening with no
 *      whole move between is a motion fault, an opening before a move not
 *      ending the count; a check's probe under the lever's hold moves
 *      nothing; the daemon's shutdown ends the sweep; switches that stop
 *      answering during the probe's lens hunt end it and the probe waits;
 *      a shutdown sends no signal to a controller over a playing kernel,
 *      and a standby does not take over a kernel whose state cannot be
 *      read
 *   V. every reap makes the head safe, after the safing pair, and so does
 *      a daemon's first spawn, once; a controller dies (SIGKILL) when the
 *      thread that forked it ends, and a child whose daemon is already gone
 *      does not run
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

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/* The supervisor's opens, passed through; a write of head/safe notes what
   the safing pair read at that moment (V). Defined after <fcntl.h>, so
   the fortified open stays the library's. */
int fake_open(const char *path, int flags, ...);
#define open fake_open

int fake_clock_gettime(clockid_t id, struct timespec *ts);
int fake_usleep(unsigned us);
unsigned fake_sleep(unsigned s);
pid_t fake_fork(void);
pid_t fake_waitpid(pid_t pid, int *status, int flags);
int fake_kill(pid_t pid, int sig);
int fake_system(const char *cmd);
int fake_flock(int fd, int op);
int fake_execle(const char *path, const char *arg, ...);
int fake_liveness_probe(int fd, char *detail, size_t dlen, int (*stop)(void));
int fake_liveness_enclosure(char *why, size_t wlen);
int fake_machine_is_idle(void);
pid_t fake_getppid(void);
#define getppid fake_getppid

#include "../src/super.c"

#include <sys/stat.h>
#include <sys/resource.h>

/* The real fork, for the cases that need a real child (V); every fork the
 * supervisor makes still goes through fake_fork. (<unistd.h> declared it
 * under the fake's name.) */
#undef fork
extern pid_t fork(void);

/* The parent a child sees, or another one (V: a daemon gone before the
 * child armed its parent-death signal). */
static int ppid_lie;
#undef getppid
extern pid_t getppid(void);
pid_t fake_getppid(void)
{
    pid_t p = getppid();
    return ppid_lie ? p + 1 : p;
}

/* The real clock, for the wait the SIGCHLD case measures. */
#undef clock_gettime
extern int clock_gettime(clockid_t clk, struct timespec *ts);

/* --- the fake clock and sleeps ---------------------------------------- */

static double fake_now = 1000.0;
static const char *create_on_sleep;     /* an attribute a sleep creates (G) */
static const char *unlink_on_kill;      /* an attribute the first signal removes (G) */
static int abort_on_sleep;              /* the lever lands this many sleeps on (F) */
static int hold_on_abort;               /* ...and holds supervision, as POST /controller/stop does */
static unsigned real_sleep_us;          /* a real pause per fake sleep (F) */

int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    return 0;
}

static void put(const char *attr, const char *v);
static long get_long(const char *attr);

int fake_usleep(unsigned us)
{
    fake_now += us / 1e6;
    if (create_on_sleep) {
        put(create_on_sleep, "0\n");
        create_on_sleep = NULL;
    }
    if (abort_on_sleep && --abort_on_sleep == 0) {
        probe_abort = 1;
        if (hold_on_abort)
            suspended |= SUSP_HOLD;
    }
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
static long kill_stop[64], kill_latch[64];  /* cnc/stop and cnc/laser_latch as each signal found them */
static int crit_logs;                   /* the safing failures named (fflog at LOG_CRIT) */
static char crit_named[64];             /* ...the write each names, in order: S cnc/stop, L cnc/laser_latch, ? another */
static int kill_crit[64];               /* ...as each signal found them */
static int reap_crit = -1;              /* ...as waitpid reported the death (-1: not yet) */
static int runner_alive;                /* pgrep -x gfhome.py answers 0 */
static int runner_term_ignored;         /* ...and SIGTERM does not end it */
static int flock_fails;
static char calls[4096];                /* the shell commands and forks, in order */

static void note(const char *what)
{
    size_t n = strlen(calls);
    snprintf(calls + n, sizeof(calls) - n, "%s;", what);
}

static int real_fork_controller;        /* the next controller fork is a real one (V) */
static int in_real_child;               /* this process is that real controller (V) */

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
        if (real_fork_controller) {
            real_fork_controller = 0;
            relay = !relay;
            pid = fork();
            if (pid == 0)
                in_real_child = 1;
            return pid;
        }
    }
    relay = !relay;
    return pid;
}

static long safe_seen_stop = -1, safe_seen_latch = -1;  /* the pair as head/safe's first write found it */
static int safe_writes;                                 /* head/safe's writes, every one */
static int safe_forks = -1;                             /* controller forks at head/safe's last write */
static int safe_probes = -1;                            /* motion probes at head/safe's last write */
static int probes;                                      /* (the probe fake's count, below) */

int fake_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    size_t lp = strlen(path), la = strlen("head/safe");
    if ((flags & O_WRONLY) && lp >= la && !strcmp(path + lp - la, "head/safe")) {
        safe_writes++;
        safe_forks = forks;
        safe_probes = probes;
        if (safe_seen_stop < 0) {
            safe_seen_stop = get_long("cnc/stop");
            safe_seen_latch = get_long("cnc/laser_latch");
        }
    }
    return openat(AT_FDCWD, path, flags, mode);
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
        reap_crit = crit_logs;          /* the notices named before the reap's own writes */
        return pid;
    }
    return 0;
}

int fake_kill(pid_t pid, int sig)
{
    if (nkills < 64) {
        kills[nkills] = sig;
        /* What the kernel attributes read at the moment of the signal:
         * the proof the safing pair came first. */
        kill_stop[nkills] = get_long("cnc/stop");
        kill_latch[nkills] = get_long("cnc/laser_latch");
        kill_crit[nkills] = crit_logs;
    }
    if (unlink_on_kill) {
        char path[160];
        snprintf(path, sizeof(path), GF_SYSFS "%s", unlink_on_kill);
        unlink(path);
        unlink_on_kill = NULL;
    }
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
    /* The real controller (V) stops here, its parent-death signal armed
     * (it comes before the exec), so the thread that forked it knows it
     * may end; it waits there to be killed. With no parent-death signal
     * the case's SIGCONT lets it go on, and SIGALRM ends it instead, which
     * the case sees. */
    if (in_real_child) {
        alarm(2);
        raise(SIGSTOP);
        for (;;)
            pause();
    }
    return -1;
}

/* --- the fake machine ------------------------------------------------- */

static int probe_rc = 1;                /* what the probe answers */
static int probe_rcs[8], nprobe_rcs, probes;    /* ...or a sequence of answers */
static int probe_led = -1;              /* the button as the last probe found it lit (-1: dark) */
static int probe_lever;                 /* the lever lands while the probe runs */
static int probe_stop_seen;             /* ...and the probe's stop input answered it */
static int open_after_probe;            /* the switches stop answering as the probe returns */
static int enable_gone_after_probe;     /* cnc/enable refuses from the first probe on */
static int enclosure_open;              /* 1 open, -1 the switches cannot be read, 0 closed */
static int enclosure_race;              /* the next enclosure read lands 1 the lever, 2 a takeover */
static int race_rc = -9;                /* ...and what it answered */
static int machine_idle = 1;
static int led_lit;                     /* the button lit by led_set, not released since */
static int led_last = -1;               /* the pattern led_set was last given */

int fake_liveness_probe(int fd, char *detail, size_t dlen, int (*stop)(void))
{
    (void)fd;
    int rc = probes < nprobe_rcs ? probe_rcs[probes] : probe_rc;
    probes++;
    note("probe");
    probe_led = led_lit ? led_last : -1;
    snprintf(detail, dlen, "fake probe %d", rc);
    if (probe_lever) {
        probe_abort = 1;
        probe_stop_seen = stop && stop();   /* the probe asks it before its move (liveness.c) */
    }
    if (open_after_probe)
        enclosure_open = -1;
    if (enable_gone_after_probe)
        unlink(GF_SYSFS "cnc/enable");
    return rc;
}

/* The read runs with the supervisor's lock down: a stop lever or a
 * check's takeover may land in it from another thread, as here. */
int fake_liveness_enclosure(char *why, size_t wlen)
{
    int race = enclosure_race;
    enclosure_race = 0;
    if (race == 1)
        race_rc = super_controller_stop();
    else if (race == 2)
        race_rc = super_takeover_begin();
    snprintf(why, wlen, "%s", enclosure_open > 0 ? "the lid is open"
                              : enclosure_open < 0 ? "the lid switches cannot be read" : "");
    return enclosure_open;
}

int fake_machine_is_idle(void)
{
    return machine_idle;
}

/* --- the stubs the supervisor links against ---------------------------- */

static char last_log[256], logs[8192];

void fflog(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (prio == LOG_CRIT) {
        if (crit_logs < (int)sizeof(crit_named) - 1)
            crit_named[crit_logs] = strstr(last_log, " cnc/stop ") ? 'S'
                                  : strstr(last_log, " cnc/laser_latch ") ? 'L' : '?';
        crit_logs++;
    }
    size_t n = strlen(logs);
    snprintf(logs + n, sizeof(logs) - n, "%s\n", last_log);
    printf("    log: %s\n", last_log);
}

/* The notices from here on: none named yet, and no reap. */
static void crit_reset(void)
{
    crit_logs = 0;
    memset(crit_named, 0, sizeof(crit_named));
    reap_crit = -1;
}

/* The write the notice at i names (0 past the record). */
static char named_at(int i)
{
    return i >= 0 && i < (int)sizeof(crit_named) ? crit_named[i] : '\0';
}

/* The two notices from i name the safing pair, the stop and the latch,
 * in either order. */
static int pair_named(int i)
{
    char a = named_at(i), b = named_at(i + 1);
    return (a == 'S' && b == 'L') || (a == 'L' && b == 'S');
}

static int engine_stops;
void cool_controller_stopped(void) { engine_stops++; }
double cool_report_age(void) { return -1.0; }
void led_set(led_pattern_t p) { led_last = (int)p; led_lit = 1; }
void led_release(void) { led_lit = 0; }
static int led_kept = -1, led_keeps;    /* the pattern led_keep was last given, and how often */
void led_keep(led_pattern_t p) { led_kept = (int)p; led_keeps++; }

/* The lens hunt: its marker, its answer, and the lever or the lid that
 * may land while it steps. It honors its stop input as lenshome.c does
 * between two steps (lenshome_test proves that half). */
static int lens_marker;                 /* run/lens.home stands */
static int lens_rc = 1;                 /* what a hunt that runs to its end answers */
static int lens_runs;                   /* hunts started */
static int lens_lever;                  /* the lever lands during the hunt, holding supervision */
static int lens_open;                   /* the switches stop answering during the hunt */
static int lens_hold_late;              /* a hold lands after the hunt's last look at its stop */
static int lens_real_lever;             /* from a thread during the hunt: 1 the lever, 2 the shutdown */
static int lens_stopped;                /* the hunt's stop input answered */
static volatile int lever_returned;     /* the real lever's call has returned */
static int lever_waited;                /* ...and it had not when the hunt ended */
static int lever_rc = -9;

static void *lever_thread(void *arg)
{
    if (arg) {
        super_shutdown();
        lever_rc = 0;
    } else {
        lever_rc = super_controller_stop();
    }
    lever_returned = 1;
    return NULL;
}

void lenshome_clear(void) { lens_marker = 0; }
int lenshome_present(void) { return lens_marker; }
int lenshome_run(char *detail, size_t dlen, int (*stop)(void))
{
    lens_runs++;
    note("lens-hunt");
    lens_marker = 0;
    if (lens_lever) {
        probe_abort = 1;
        suspended |= SUSP_HOLD;
    }
    if (lens_open)
        enclosure_open = -1;
    if (lens_real_lever) {
        /* The lever arrives while the hunt steps, and the hunt asks its
         * stop between steps, a millisecond apart, for up to 2 s. */
        pthread_t t;
        lever_returned = 0;
        pthread_create(&t, NULL, lever_thread, lens_real_lever == 2 ? (void *)1 : NULL);
        pthread_detach(t);
        for (int i = 0; i < 2000 && !(stop && stop()); i++) {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
        struct timespec ts = { 0, 20000000 };
        nanosleep(&ts, NULL);           /* a lever that does not wait has returned by now */
        lever_waited = !lever_returned;
    }
    if (stop && stop()) {
        lens_stopped = 1;
        snprintf(detail, dlen, "fake hunt ended");
        return LENSHOME_STOPPED;
    }
    if (lens_hold_late)
        suspended |= SUSP_HOLD;
    snprintf(detail, dlen, "fake hunt %d", lens_rc);
    lens_marker = lens_rc == 1;
    return lens_rc;
}
void cam_lamp_apply_idle(void) { }
int setup_gate_open(char *why, size_t len) { if (len) why[0] = '\0'; return 1; }
static int dev_image;                   /* the image carries the dev marker (M) */
int auth_dev_image(void) { return dev_image; }
int diag_running(void) { return 0; }
int update_job_running(void) { return 0; }
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }
static int cloud_enabled;
int settings_get_bool(const char *key, int def) { return !strcmp(key, "cloud_enabled") ? cloud_enabled : def; }
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

#define RELEASE_MARKER "super-test/run/motors.released"

static void make_tree(void)
{
    mkdir("super-test", 0755);
    mkdir("super-test/dev", 0755);
    mkdir("super-test/data", 0755);
    mkdir(GF_SYSFS, 0755);
    mkdir(GF_SYSFS "cnc", 0755);
    mkdir(GF_SYSFS "head", 0755);
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    put("cnc/halt", "0\n");
    put("cnc/enable", "0\n");
    put("cnc/disable", "0\n");
    put("cnc/state", "idle\n");
    put("cnc/faults", "0\n");
    put("head/safe", "0\n");
    FILE *f = fopen(PULSE_DEV, "w");
    if (f)
        fclose(f);
    setenv("FORGECTRL_DATA_DIR", "super-test/data", 1);
    mkdir("super-test/run", 0755);
    setenv("GF_RUN_DIR", "super-test/run", 1);
    unlink(RELEASE_MARKER);
}

static void clear_safing(void)
{
    put("cnc/stop", "0\n");
    put("cnc/laser_latch", "0\n");
    put("cnc/halt", "0\n");
    put("head/safe", "0\n");
    safe_seen_stop = safe_seen_latch = -1;
}

/* The kernel at rest, its rail writes forgotten. */
static void kernel_at_rest(void)
{
    put("cnc/state", "idle\n");
    put("cnc/faults", "0\n");
    put("cnc/enable", "0\n");
    put("cnc/disable", "0\n");
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
    crit_reset();
    unlink_on_kill = NULL;
    led_last = -1;
    led_lit = 0;
    probe_led = -1;
    probe_lever = 0;
    open_after_probe = 0;
    enable_gone_after_probe = 0;
    enclosure_race = 0;
    race_rc = -9;
    abort_on_sleep = 0;
    hold_on_abort = 0;
    lens_marker = 0;
    lens_rc = 1;
    lens_runs = 0;
    lens_lever = 0;
    lens_open = 0;
    lens_hold_late = 0;
    lens_real_lever = 0;
    lens_stopped = 0;
    lever_waited = 0;
    lever_rc = -9;
    probe_stop_seen = 0;
    pthread_mutex_lock(&mu);
    sweep_failed = 0;
    moves_stopped = 0;
    pthread_mutex_unlock(&mu);
    calls[0] = '\0';
    logs[0] = '\0';
    clear_safing();
    kernel_at_rest();
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

/* A check takes the machine from a running controller: the stop the
 * lifecycle thread makes once the takeover is asked for, then the ask,
 * which finds the controller gone. */
static int take_over(void)
{
    pthread_mutex_lock(&mu);
    suspended |= SUSP_TAKEOVER;
    stop_locked();
    pthread_mutex_unlock(&mu);
    return super_takeover_begin();
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

/* A cloud controller spawned from this thread (no data directory to enter
 * first), which ends once the child has stopped at its exec (V). */
static void *spawn_from_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    spawn_locked(Ctl_Cloud);
    pid_t c = child_pid;
    pthread_mutex_unlock(&mu);
    int st;
    if (c > 0)
        wait4(c, &st, WUNTRACED, NULL);
    return NULL;
}

int main(void)
{
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
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

    printf("D2. the first spawn waits for the lid the same way\n");
    reset_state();
    enclosure_open = 1;
    CHECK(!run_until_spawned(3.0), "an open lid holds the first spawn");
    CHECK(probes == 0 && forks == 0 && child_pid == 0, "no motion probe, no fork, no pid");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(enclosure_wait && !strcmp(wait_why, "the lid is open") &&
              strstr(st, "\"controller\":\"waiting\"") != NULL, "/mode says waiting, and that the lid is open");
    }
    CHECK(led_last == LED_BLINK_AMBER, "the button blinks amber");
    {
        /* The amber is written once; the cooling engine's red flash stands
         * over it and ends dark, so every pass of the wait keeps it. */
        int keeps = led_keeps;
        tick();
        tick();
        CHECK(led_keeps == keeps + 2 && led_kept == LED_BLINK_AMBER, "and every pass of the wait keeps the amber");
    }
    enclosure_open = 0;
    tick();
    CHECK(probes == 1 && child_pid == 0, "the lid closed: the probe runs first");
    CHECK(run_until_spawned(1.0) && forks == 1 && probes == 1, "then the spawn");

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
    double t_lever = fake_now;
    int rc = probe_sequence(3);
    pthread_mutex_lock(&mu);
    probing = 0;
    pthread_mutex_unlock(&mu);
    CHECK(rc == 4, "the sequence ends with the lever's code");
    CHECK(get_long("cnc/stop") == 1, "with motion stopped");
    CHECK(probes == 1, "before the second rung probed");
    CHECK(fake_now - t_lever < 5.0, "within the rail-off wait, not at its end");
    CHECK(strstr(probe_detail, "stop lever") != NULL, "and says why");
    reset_state();
    probe_rc = 0;
    probe_lever = 1;                    /* the lever lands before the head has moved */
    rc = probe_sequence(3);
    CHECK(rc == 4 && probes == 1 && get_long("cnc/disable") == 0,
          "a lever that cuts a probe short ends the sequence there: no rail-off rung follows");
    CHECK(probe_stop_seen, "the probe's own stop input answers the lever, before its move");
    reset_state();
    lens_lever = 1;                     /* the lever lands during the lens hunt */
    rc = probe_sequence(3);
    CHECK(rc == 4 && lens_runs == 1 && !lens_marker, "the lever reaches the lens hunt: it ends, no marker");
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
    crit_reset();
    CHECK(wr_attr("cnc/stop", "1") == -1 && crit_logs == 1 && strstr(last_log, "3 times") != NULL,
          "three failures are named once, at the highest severity");
    put("cnc/stop", "0\n");
    bring_up();
    unlink(GF_SYSFS "cnc/laser_latch");
    crit_reset();
    unlink_on_kill = "cnc/stop";        /* the stop attribute goes at the first signal */
    pthread_mutex_lock(&mu);
    suspended = 1;
    stop_locked();
    pthread_mutex_unlock(&mu);
    CHECK(child_pid == 0 && nkills >= 2 && kills[0] == SIGTERM && kill_stop[0] == 1,
          "the stop signals and reaps the controller, motion stopped before the signal");
    CHECK(strstr(logs, "laser_latch failed") != NULL, "a latch write that keeps failing is named");
    /* Each notice names its write: the latch before the signal; the
     * stop and the latch after it, before the reap; the stop and the
     * latch at the reap. Within a pair the order is free. */
    int at_signal = kill_crit[0], at_reap = reap_crit;
    printf("    notices: %s (%d before the signal, %d before the reap)\n", crit_named, at_signal, at_reap);
    CHECK(at_signal == 1 && named_at(0) == 'L',
          "the latch write before the signal is named, and no other write");
    CHECK(at_reap - at_signal == 2 && pair_named(at_signal),
          "the pair is tried again after the signal: the stop and the latch named before the reap");
    CHECK(at_reap >= 0 && crit_logs - at_reap == 2 && pair_named(at_reap),
          "and once more at the reap: the stop and the latch");
    put("cnc/laser_latch", "0\n");
    put("cnc/stop", "0\n");

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
    /* The default device is there now: only the override can refuse. */
    reset_state();
    char cwd[512], elsewhere[640];
    snprintf(elsewhere, sizeof elsewhere, "%s/super-test/dev/elsewhere",
             getcwd(cwd, sizeof cwd) ? cwd : ".");
    setenv("GF_PULSE_DEV", elsewhere, 1);
    CHECK(!run_until_spawned(4.0), "a GF_PULSE_DEV that is not there starts no controller");
    CHECK(strstr(logs, elsewhere) != NULL && crit_logs == 1, "the refusal names the path it tried");
    reset_state();
    setenv("GF_PULSE_DEV", "super-test/dev/glowforge", 1);  /* the device that is there, by a relative path */
    CHECK(!run_until_spawned(4.0) && broker_fd < 0,
          "a GF_PULSE_DEV that is not an absolute path starts no controller");
    CHECK(strstr(logs, "not an absolute path") != NULL && crit_logs == 1, "the refusal names the rule, once");
    unsetenv("GF_PULSE_DEV");

    printf("I. the engine's fail tier stops the controller and starts it again\n");
    bring_up();
    pid_t before = child_pid;
    super_controller_restart("head crash signal");
    CHECK(restart_pending, "the request is posted");
    tick();
    CHECK(child_pid == 0 || child_pid != before, "the controller was stopped");
    CHECK(nkills >= 1 && kills[0] == SIGTERM && kill_stop[0] == 1 && kill_latch[0] == 1,
          "the deliberate way: the SIGTERM found motion stopped and the latch locked");
    CHECK(safed(), "and both stand after it");
    CHECK(engine_stops >= 1, "the engine forgets the reporter");
    CHECK(run_until_spawned(1.0) && child_pid != before, "a new controller follows without a backoff");
    CHECK(probes == 1, "and without a second motion probe");

    printf("J. a motor release is not ended from here\n");
    reset_state();
    put("cnc/enable", "0\n");           /* the cases before this one probed */
    put("cnc/disable", "0\n");
    {
        FILE *f = fopen(RELEASE_MARKER, "w");
        if (f)
            fclose(f);
    }
    tick();                             /* the probe's turn, the lid closed */
    int probe_turn = probed && lens_runs == 0 && forks == 0;
    enclosure_open = 1;                 /* the lid opens before the sweep's turn */
    CHECK(probe_turn && !run_until_spawned(3.0) && lens_runs == 0 && enclosure_wait,
          "an open lid holds the lens sweep under the release, and the spawn");
    enclosure_open = 0;
    CHECK(run_until_spawned(5.0), "the controller still comes up: it is what takes the release over");
    CHECK(probes == 0 && lens_runs == 1,
          "the motion probe never ran; the lens was swept (Z alone: the lens motor is not released)");
    CHECK(get_long("cnc/enable") == 0 && get_long("cnc/disable") == 0, "and nothing was written to the rail");
    CHECK(probed && probe_skipped, "motion is unverified, not faulted");
    {
        char st[256], err[128] = "";
        cloud_enabled = 1;
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "\"motion\":\"unverified\"") != NULL && strstr(st, "released") != NULL,
              "and /mode says why");
        CHECK(super_mode_switch("cloud", NULL, err, sizeof(err)) == -1 && strstr(err, "released") != NULL,
              "a switch to cloud mode is refused, in words");
        CHECK(want == Ctl_Grbl, "the mode did not change");
    }
    cloud_enabled = 0;
    unlink(RELEASE_MARKER);
    reset_state();
    CHECK(run_until_spawned(5.0) && probes == 1 && !probe_skipped,
          "with the marker gone the probe runs as always");

    printf("K. the machine lease refuses a mode switch, except its holder's own\n");
    reset_state();
    CHECK(run_until_spawned(5.0), "the controller is up");
    {
        char err[128] = "";
        CHECK(lease_take("wizard:cloud.header", LEASE_HARDWARE, NULL, err, sizeof(err)) == 0, "the wizard takes the lease");
        CHECK(super_mode_switch("grbl", NULL, err, sizeof(err)) == -1 &&
              !strcmp(err, "a setup wizard (cloud.header) holds the machine"),
              "anybody else's switch is refused, and the refusal names the holder");
        CHECK(want == Ctl_Grbl, "the mode did not change");
        /* The holder's own switch gets past the lease: with the machine
         * busy it is the idle check that answers, which comes after. */
        machine_idle = 0;
        CHECK(super_mode_switch("grbl", "wizard:cloud.header", err, sizeof(err)) == -1 &&
              !strcmp(err, "machine is not idle"), "the holder's own switch is not refused by its own hold");
        CHECK(super_mode_switch("grbl", "wizard:motion", err, sizeof(err)) == -1 && strstr(err, "holds the machine"),
              "a caller that names a hold it does not have is refused");
        machine_idle = 1;
        lease_release("wizard:cloud.header");
        machine_idle = 0;
        CHECK(super_mode_switch("grbl", NULL, err, sizeof(err)) == -1 && !strcmp(err, "machine is not idle"),
              "with the lease given back the switch reaches the idle check again");
        machine_idle = 1;
    }

    printf("L. the report channel is the running controller's alone\n");
    reset_state();
    CHECK(!super_report_secret_ok("") && !super_report_secret_ok(NULL) &&
          !super_report_secret_ok("00000000000000000000000000000000"), "with no controller nothing passes");
    setenv("GF_REPORT_SECRET", "0123456789abcdef0123456789abcdef", 1);  /* a stale one, inherited */
    CHECK(run_until_spawned(5.0), "the controller is up");
    {
        char first[REPORT_SECRET_HEX + 1], handed[64] = "";
        snprintf(first, sizeof(first), "%s", report_secret);
        CHECK(strlen(first) == REPORT_SECRET_HEX && strspn(first, "0123456789abcdef") == REPORT_SECRET_HEX,
              "the spawn made a secret of 32 hex digits");
        char **env = build_child_env(Ctl_Grbl, 5);
        int n = 0;
        for (size_t i = 0; env && env[i]; i++)
            if (!strncmp(env[i], "GF_REPORT_SECRET=", 17)) {
                snprintf(handed, sizeof(handed), "%s", env[i] + 17);
                n++;
            }
        free_child_env(env);
        CHECK(n == 1 && !strcmp(handed, first), "the child's environment carries it once, the inherited one gone");
        CHECK(super_report_secret_ok(first), "the secret passes");
        CHECK(!super_report_secret_ok("0123456789abcdef0123456789abcdef"), "the inherited one does not");
        char near[REPORT_SECRET_HEX + 2];
        snprintf(near, sizeof(near), "%s", first);
        near[7] = near[7] == '0' ? '1' : '0';
        CHECK(!super_report_secret_ok(near), "one digit off does not");
        snprintf(near, sizeof(near), "%s0", first);
        CHECK(!super_report_secret_ok(near), "nor the secret with more behind it");
        near[REPORT_SECRET_HEX - 1] = '\0';
        CHECK(!super_report_secret_ok(near) && !super_report_secret_ok(""), "nor a part of it, nor nothing");
        CHECK(!strstr(logs, first), "the secret is in no log line");

        CHECK(super_grbl_pid() == (long)child_pid, "the M-code relay's key is the running controller's pid");
        die(0x0900);
        tick();
        CHECK(child_pid == 0 && !super_report_secret_ok(first), "a reap ends it");
        CHECK(super_grbl_pid() == 0, "and with no controller the key is none");
        CHECK(run_until_spawned(30.0), "the respawn comes up");
        CHECK(strcmp(report_secret, first) && strlen(report_secret) == REPORT_SECRET_HEX &&
              !super_report_secret_ok(first) && super_report_secret_ok(report_secret),
              "with a secret of its own, and the old one stays dead");
    }
    unsetenv("GF_REPORT_SECRET");

    printf("M. a dev image's controllers wait on the boot's data record\n");
    {
        static const char *const record = "super-test/run/data.state";
        static const struct {
            const char *word;           /* NULL: no record */
            int starts;
        } boots[] = {
            { "emmc\n", 1 }, { "card\n", 1 }, { "card-none\n", 1 }, { NULL, 0 }, { "card-mounted\n", 0 },
        };
        setenv("GF_DATA_STATE", record, 1);
        dev_image = 1;
        for (size_t i = 0; i < sizeof(boots) / sizeof(*boots); i++) {
            const char *w = boots[i].word;
            char msg[96], name[24], body[512];
            snprintf(name, sizeof(name), "%.*s", w ? (int)strcspn(w, "\n") : 0, w ? w : "");
            reset_state();
            unlink(record);
            if (w) {
                FILE *f = fopen(record, "w");
                if (f) {
                    fputs(w, f);
                    fclose(f);
                }
            }
            int spawned = run_until_spawned(5.0);
            super_status_json(body, sizeof(body));
            int held = strstr(body, "\"gated\":true") != NULL;
            snprintf(msg, sizeof(msg), "the record %s: %s", w ? name : "missing",
                     boots[i].starts ? "a controller starts" : "no controller, and /mode says gated");
            CHECK(boots[i].starts ? spawned && !held : !spawned && held && forks == 0, msg);
        }
        dev_image = 0;
        reset_state();
        unlink(record);
        CHECK(run_until_spawned(5.0), "a release image with no record starts its controller");
        unsetenv("GF_DATA_STATE");
    }

    printf("N. the button while the probe moves the machine; the motion fault the cooling engine reads\n");
    reset_state();
    CHECK(run_until_spawned(5.0) && probes == 1 && probe_led == LED_SOLID_WHITE && !led_lit,
          "solid white while the probe moves the machine, put out after it");
    reset_state();
    probe_rc = 0;                       /* NO MOTION at every rung of the ladder */
    tick();
    CHECK(probes == 4 && super_motion_fault() == 1, "no motion after the ladder: the fault, as the engine reads it");
    for (int i = 0; i < 10; i++) {
        tick();
        fake_now += 1.0;
    }
    CHECK(probes == 4 && forks == 0, "ten passes later: no probe and no controller");
    probe_rc = 1;
    super_controller_start();           /* the operator's retry */
    CHECK(super_motion_fault() == 0, "the retry clears it");
    tick();
    CHECK(probes == 5 && forks == 0, "the pass after the retry probes before any controller starts");
    reset_state();

    printf("O. the gate's enclosure inputs, and a probe the enclosure ends\n");
    reset_state();
    enclosure_open = -1;
    CHECK(!run_until_spawned(3.0) && probes == 0 && forks == 0,
          "switches that cannot be read: no probe and no controller");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "\"controller\":\"waiting\"") && strstr(st, "cannot be read"),
              "/mode says waiting, and that the switches cannot be read");
    }
    reset_state();
    probe_rcs[0] = -2;                  /* the lid opened under the probe */
    probe_rcs[1] = 1;
    nprobe_rcs = 2;
    tick();
    CHECK(probes == 1 && forks == 0 && !probed, "a probe the enclosure ended decides nothing, starts nothing");
    CHECK(run_until_spawned(3.0) && probes == 2 && probed && !probe_skipped,
          "the next probe runs, and the controller starts behind it");
    reset_state();
    open_after_probe = 1;               /* the lid opens between the probe and the lens hunt */
    tick();
    CHECK(probes == 1 && lens_runs == 0 && forks == 0 && !probed,
          "the lens does not move with the lid open: the probe waits");
    open_after_probe = 0;
    enclosure_open = 0;
    CHECK(run_until_spawned(3.0) && probes == 2 && lens_runs == 1 && lens_marker,
          "with the lid closed: the probe, the lens, the controller");

    printf("P. a probe that fails on the machine, and a lens with no edge\n");
    reset_state();
    probe_rc = -4;
    tick();
    CHECK(probes == 1 && super_motion_fault() && get_long("cnc/disable") == 0 && forks == 0,
          "a probe that fails on the machine is a motion fault at once: no rail cycle, no controller");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "fake probe -4") != NULL, "/mode carries the probe's words");
    }
    reset_state();
    lens_rc = 0;                        /* the hunt finds no hall edge */
    CHECK(!run_until_spawned(3.0) && forks == 0 && super_motion_fault(),
          "a lens with no edge behind a moving probe is a motion fault: no controller");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "\"why\":\"lens:") != NULL, "/mode says it is the lens");
    }

    printf("Q. the kernel as the last holder left it\n");
    reset_state();
    put("cnc/state", "fault\n");
    put("cnc/faults", "2\n");
    unlink(GF_SYSFS "cnc/enable");      /* the kernel refuses the enable: a line still asserted */
    tick();
    CHECK(probes == 0 && super_motion_fault() && forks == 0 && get_long("cnc/disable") == 0,
          "a stepper fault the enable cannot clear is a motion fault: no probe, no rail cycle");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "fault") && strstr(st, "faults 2"), "/mode names the state and the lines");
    }
    int crits = crit_logs;
    for (int i = 0; i < 10; i++) {
        tick();
        fake_now += 2.0;
    }
    CHECK(crit_logs == crits && probes == 0, "and the enable is not tried again until a retry");
    reset_state();
    put("cnc/state", "fault\n");
    put("cnc/faults", "1\n");           /* latched, its line cleared */
    CHECK(run_until_spawned(3.0) && get_long("cnc/enable") == 1 && probes == 1,
          "a stepper fault whose lines have cleared: the one enable, then the probe and the controller");
    CHECK(strstr(logs, "faults 1") != NULL, "the mask it cleared is logged");
    reset_state();
    put("cnc/state", "underrun\n");
    CHECK(run_until_spawned(3.0) && get_long("cnc/stop") == 1 && probes == 1,
          "an underrun left behind is acknowledged before the probe");
    reset_state();
    put("cnc/state", "running\n");
    alarm(20);                          /* an unbounded wait fails here, not in CI's timeout */
    double t_busy = fake_now;
    CHECK(run_until_spawned(30.0) && get_long("cnc/halt") == 1 && probes == 1,
          "a kernel still playing at the probe is waited for, then halted, then probed");
    alarm(0);
    CHECK(fake_now - t_busy >= PROBE_BUSY_WAIT_S && strstr(logs, "halting it") && crit_logs > 0,
          "after the bound, said at the highest severity");
    bring_up();
    put("cnc/state", "fault\n");
    put("cnc/faults", "4\n");
    machine_idle = 0;
    die(0x0900);
    tick();
    fake_now += 2.0;
    CHECK(!run_until_spawned(15.0) && get_long("cnc/halt") == 0 && super_motion_fault() && !probed,
          "a respawn over a stepper fault: no halt, no controller, a motion fault");
    {
        char st[256];
        super_status_json(st, sizeof(st));
        CHECK(strstr(st, "faults 4") && strstr(st, "\"stepper_fault\":true"),
              "/mode names the lines, and a stepper fault for the panel's Retry");
    }
    machine_idle = 1;
    put("cnc/faults", "0\n");
    put("cnc/enable", "0\n");
    super_controller_start();           /* the retry, once the lines have cleared */
    int before_probes = probes;
    CHECK(run_until_spawned(3.0) && probes == before_probes + 1 && get_long("cnc/enable") == 1,
          "the retry probes, with its one enable, before the controller starts");
    bring_up();
    put("cnc/state", "fault\n");
    machine_idle = 0;
    nkills = 0;
    super_shutdown();
    CHECK(nkills >= 1 && kills[0] == SIGTERM && child_pid == 0,
          "a shutdown stops a controller over a stepper fault: it plays nothing");
    machine_idle = 1;
    reset_state();
    put("cnc/state", "fault\n");
    machine_idle = 0;
    standby_takeover = 1;
    suspended = SUSP_STANDBY;
    tick();
    CHECK(strstr(calls, "/etc/init.d/grblhal stop") && !standby_takeover && !suspended,
          "standing by over a stepper fault: the machine is taken over");
    machine_idle = 1;

    printf("R. a check's hand-back is its own; the check's probe\n");
    bring_up();
    CHECK(take_over() == 0 && child_pid == 0, "a check takes the machine");
    CHECK(super_controller_stop() == 0, "the operator pulls the stop lever during it");
    super_takeover_end();
    int f0 = forks;
    CHECK(!run_until_spawned(3.0) && forks == f0, "the check's hand-back leaves the operator's stop standing");
    super_controller_start();
    CHECK(run_until_spawned(3.0), "the operator's start brings the controller back");
    reset_state();
    probe_rc = 0;
    CHECK(super_takeover_begin() == 0, "a check takes the machine");
    {
        char d[96];
        CHECK(super_probe_motion(d, sizeof(d)) == 0 && super_motion_fault(), "its probe finds no motion");
    }
    super_takeover_end();
    int p0 = probes;
    for (int i = 0; i < 10; i++) {
        tick();
        fake_now += 1.0;
    }
    CHECK(probes == p0 && forks == 0 && super_motion_fault(),
          "the motion fault it found outlives the hand-back: no ladder, no controller");
    bring_up();                         /* motion verified at the boot */
    CHECK(take_over() == 0, "a check takes the machine");
    probe_rcs[0] = 0;                   /* the check's probe: no motion, then the lid open */
    probe_rcs[1] = -2;
    nprobe_rcs = 2;
    probes = 0;
    {
        char d[96];
        CHECK(super_probe_motion(d, sizeof(d)) == 3 && !probed, "a check's probe the lid ends leaves motion unverified");
    }
    super_takeover_end();
    probe_rc = 1;
    nprobe_rcs = 0;
    probes = 0;
    forks = 0;
    calls[0] = '\0';
    CHECK(run_until_spawned(3.0) && probes == 1 && strstr(calls, "probe") < strstr(calls, "fork-controller"),
          "the lifecycle probes from the first rung before the controller starts");
    bring_up();
    CHECK(take_over() == 0, "a check takes the machine");
    probe_rc = 0;
    abort_on_sleep = 12;                /* the operator's stop lands in the rail-off wait */
    hold_on_abort = 1;
    probes = 0;
    {
        char d[96];
        CHECK(super_probe_motion(d, sizeof(d)) == 4 && !probed, "the lever ends the check's probe");
    }
    super_takeover_end();
    probe_rc = 1;
    probes = 0;
    forks = 0;
    CHECK(!run_until_spawned(3.0) && probes == 0 && forks == 0,
          "the check's hand-back: the operator's stop stands, nothing moves, nothing starts");
    reset_state();
    runner_alive = 1;                   /* a stopped controller's homing runner */
    {
        char d[96];
        super_probe_motion(d, sizeof(d));
    }
    CHECK(strstr(calls, "pkill -TERM -x gfhome.py") && strstr(calls, "pkill -TERM -x gfhome.py") < strstr(calls, "probe"),
          "the check's probe ends a homing runner before it writes the ring");

    printf("S. the lens reference goes with every controller and comes back before the next\n");
    bring_up();
    CHECK(lens_marker && lens_runs == 1, "the probe's hunt referenced the lens");
    die(0x0900);
    tick();
    CHECK(child_pid == 0 && !lens_marker, "the reap drops the marker");
    calls[0] = '\0';
    CHECK(run_until_spawned(5.0) && lens_runs == 2 && lens_marker &&
          strstr(calls, "lens-hunt") && strstr(calls, "lens-hunt") < strstr(calls, "fork-controller"),
          "the respawn sweeps the lens onto its edge before the controller starts");
    die(0x0900);
    tick();
    lens_rc = 0;                        /* the sweep finds no edge */
    int l1 = lens_runs;
    CHECK(run_until_spawned(10.0) && lens_runs == l1 + 1 && !lens_marker && !super_motion_fault(),
          "a sweep that finds no edge: the controller starts, Z unreferenced, no motion fault");
    die(0x0900);
    tick();
    CHECK(run_until_spawned(10.0) && lens_runs == l1 + 1, "and no start after it in the hold sweeps again");
    CHECK(take_over() == 0, "a check takes the machine");
    lens_marker = 1;                    /* its hunt finds the edge */
    super_takeover_end();
    CHECK(run_until_spawned(5.0) && lens_runs == l1 + 1, "the controller comes back on the check's reference");
    lens_rc = 1;
    die(0x0900);
    tick();
    CHECK(run_until_spawned(10.0) && lens_runs == l1 + 2 && lens_marker,
          "a hunt that found the edge lets the sweep run again");
    reset_state();
    bring_up();
    die(0x0900);
    tick();
    fake_now += 2.0;                    /* past the respawn's backoff */
    lens_real_lever = 1;                /* the operator's stop, from its own thread, while the sweep steps */
    real_sleep_us = 1000;
    f0 = forks;
    tick();
    real_sleep_us = 0;
    for (int i = 0; i < 1000 && !lever_returned; i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    CHECK(lens_stopped && lever_waited && lever_rc == 0 && forks == f0 && !lens_marker,
          "the lever reaches the sweep between two steps, and returns only once the sweep has ended; no controller");
    lens_real_lever = 0;
    reset_state();
    bring_up();
    die(0x0900);
    tick();
    lens_hold_late = 1;                 /* a hold lands after the sweep's last step */
    f0 = forks;
    CHECK(!run_until_spawned(5.0) && forks == f0, "a hold after the sweep's last step: no controller");
    reset_state();
    bring_up();
    die(0x0900);
    tick();
    fake_now += 2.0;
    lens_open = 1;                      /* the switches stop answering during the sweep */
    f0 = forks;
    tick();
    CHECK(lens_stopped && forks == f0 && enclosure_wait,
          "switches that stop answering during the sweep end it, and the spawn waits for them");
    lens_open = 0;
    enclosure_open = 0;
    CHECK(run_until_spawned(3.0), "the controller starts once they read closed");
    bring_up();
    int l0 = lens_runs;
    pthread_mutex_lock(&mu);
    want = Ctl_Cloud;
    pthread_mutex_unlock(&mu);
    die(0x0900);
    tick();
    CHECK(run_until_spawned(5.0) && lens_runs == l0, "the cloud client homes the lens itself: no sweep before it");
    reset_state();

    printf("T. a lever or a takeover in the pass's enclosure read: nothing moves, nothing starts\n");
    bring_up();
    die(0x0900);
    tick();
    fake_now += 5.0;                    /* past the respawn's backoff */
    int l2 = lens_runs;
    f0 = forks;
    enclosure_race = 1;
    tick();
    CHECK(race_rc == 0 && lens_runs == l2 && forks == f0,
          "the lever answered stopped, and no sweep stepped the lens after it, no controller started");
    super_controller_start();
    CHECK(run_until_spawned(5.0) && lens_runs == l2 + 1, "the operator's start: the sweep, then the controller");
    die(0x0900);
    tick();
    fake_now += 5.0;
    l2 = lens_runs;
    f0 = forks;
    enclosure_race = 2;
    tick();
    CHECK(race_rc == 0 && lens_runs == l2 && forks == f0,
          "a check's takeover: the check has the machine, and no sweep steps the lens under it");
    super_takeover_end();
    CHECK(run_until_spawned(5.0), "its hand-back brings the controller back");
    reset_state();

    printf("U. what the kernel's state, the switches and the bounds leave alone\n");
    reset_state();
    put("cnc/state", "running\n");
    abort_on_sleep = 5;                 /* the lever lands in the wait for the busy kernel */
    double t_wait = fake_now;
    rc = probe_sequence(3);
    CHECK(rc == 4 && get_long("cnc/halt") == 0 && fake_now - t_wait < PROBE_BUSY_WAIT_S,
          "the lever reaches the probe's wait for a busy kernel: it ends there, nothing halted");
    reset_state();
    probe_rc = 0;                       /* no motion at the first rung */
    enable_gone_after_probe = 1;        /* the second rung's enable is refused */
    tick();
    CHECK(probes == 1 && super_motion_fault(),
          "an enable refused at a later rung (a fault latched with the rail off) is a motion fault, no second probe");
    reset_state();
    probe_rc = 0;                       /* the ladder ends with no motion... */
    tick();
    {
        char st[256];
        super_status_json(st, sizeof(st));
        int calm = strstr(st, "\"stepper_fault\":false") != NULL;
        put("cnc/state", "fault\n");    /* ...and a driver fault latched during its last move */
        super_status_json(st, sizeof(st));
        CHECK(calm && super_motion_fault() && strstr(st, "\"stepper_fault\":true"),
              "/mode names a stepper fault by what the kernel holds, however the motion fault was found");
    }
    reset_state();
    put("cnc/state", "fault\n");
    unlink(GF_SYSFS "cnc/enable");      /* the check's probe meets a refused enable */
    {
        char d[96], st[256];
        int prc = super_probe_motion(d, sizeof(d));
        super_status_json(st, sizeof(st));
        CHECK(prc == 0 && strstr(st, "\"stepper_fault\":true"),
              "a check's probe whose enable the kernel refuses: /mode names a stepper fault");
    }
    reset_state();
    probe_rc = -5;                      /* the enclosure opens under every probe's move */
    run_until_spawned(10.0);
    CHECK(probes == PROBE_STOPPED_MAX && super_motion_fault() && forks == 0,
          "a third probe in a row stopped under its move is a motion fault: the head walks no further");
    reset_state();
    {
        static const int alt[] = { -5, -2, -5, -2, -5 };    /* a lid that flickers through the starts */
        for (int i = 0; i < 5; i++)
            probe_rcs[i] = alt[i];
        nprobe_rcs = 5;
    }
    run_until_spawned(15.0);
    CHECK(probes == 5 && super_motion_fault() && forks == 0,
          "an opening before a move does not end the count: the third stopped move is the fault");
    reset_state();
    {
        static const int whole[] = { -5, -5, 1, -5 };       /* a whole move between stops */
        for (int i = 0; i < 4; i++)
            probe_rcs[i] = whole[i];
        nprobe_rcs = 4;
        char d[96];
        int r1 = super_probe_motion(d, sizeof(d)), r2 = super_probe_motion(d, sizeof(d));
        int r3 = super_probe_motion(d, sizeof(d)), r4 = super_probe_motion(d, sizeof(d));
        CHECK(r1 == 3 && r2 == 3 && r3 == 1 && r4 == 3 && !super_motion_fault(),
              "a move that ran whole ends the count: the stop after it is the first again");
    }
    reset_state();
    CHECK(super_controller_stop() == 0, "the operator's stop, with no controller running");
    {
        char d[96];
        CHECK(super_probe_motion(d, sizeof(d)) == 4 && probes == 0,
              "a check's probe after the stop lever answered: rc 4, nothing moved");
    }
    reset_state();
    bring_up();
    die(0x0900);
    tick();
    fake_now += 2.0;
    lens_real_lever = 2;                /* the daemon's shutdown, from its own thread, during the sweep */
    real_sleep_us = 1000;
    tick();
    real_sleep_us = 0;
    for (int i = 0; i < 1000 && !lever_returned; i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    CHECK(lens_stopped && !lens_marker, "the daemon's shutdown ends the sweep between two steps");
    lens_real_lever = 0;
    reset_state();
    lens_open = 1;                      /* the switches stop answering during the probe's lens hunt */
    rc = probe_sequence(3);
    CHECK(rc == 3 && lens_stopped, "the probe's lens hunt ends on switches that stop answering, and the probe waits");
    bring_up();
    put("cnc/state", "running\n");
    nkills = 0;
    super_shutdown();
    CHECK(nkills == 0 && child_pid > 0,
          "a shutdown sends no signal to a controller whose kernel plays (it ends with the lifecycle thread, V)");
    reset_state();
    unlink(GF_SYSFS "cnc/state");
    standby_takeover = 1;
    suspended = SUSP_STANDBY;
    tick();
    CHECK(standby_takeover && !strstr(calls, "/etc/init.d/grblhal stop"),
          "standing by over a kernel whose state cannot be read: no takeover");
    reset_state();

    printf("V. the head made safe at every reap; a controller dies with the thread that forked it\n");
    bring_up();
    die(0x0900);
    tick();
    CHECK(child_pid == 0 && get_long("head/safe") == 1,
          "the reap makes the head safe: emitters off, the lens driver disabled");
    CHECK(safe_seen_stop == 1 && safe_seen_latch == 1, "after the safing pair: motion stopped and the latch locked first");
    {
        int n;
        /* A daemon's start with no unmanaged controller: the head made
         * safe there, before the lifecycle thread's probe and any fork. */
        reset_state();
        pthread_mutex_lock(&mu);
        head_safed = 0;
        n = safe_writes;
        init_locked();
        int safed_at_start = head_safed;
        pthread_mutex_unlock(&mu);
        CHECK(safe_writes == n + 1 && safed_at_start && safe_probes == 0 && safe_forks == 0,
              "a daemon's start makes the head safe before the probe and the sweep");
        n = safe_writes;
        CHECK(run_until_spawned(5.0) && safe_writes == n, "its first spawn writes it no second time");
        /* One that found an unmanaged controller leaves that one's head
         * alone, and makes it safe before its own first fork. */
        reset_state();
        runner_alive = 1;
        pthread_mutex_lock(&mu);
        head_safed = 0;
        n = safe_writes;
        init_locked();
        int standing_by = standby_takeover && !head_safed;
        standby_takeover = 0;
        suspended = 0;
        pthread_mutex_unlock(&mu);
        runner_alive = 0;
        CHECK(standing_by && safe_writes == n, "an unmanaged controller at the start: its head is left alone");
        n = safe_writes;
        bring_up();
        CHECK(safe_writes == n + 1 && safe_forks == 0,
              "the daemon's first spawn makes the head safe first, once, before the fork");
        n = safe_writes;
        die(0x0900);
        tick();
        pthread_mutex_lock(&mu);
        respawn_at = 0.0;
        pthread_mutex_unlock(&mu);
        CHECK(run_until_spawned(5.0) && safe_writes == n + 1, "a respawn of the same daemon: only the reap's");
    }
    reset_state();
    bring_up();
    {
        int n = safe_writes;
        put("head/safe", "0\n");
        pthread_mutex_lock(&mu);
        suspended |= SUSP_HOLD;         /* the stop lever, as the pass stops the controller for it */
        stop_locked();
        pthread_mutex_unlock(&mu);
        CHECK(child_pid == 0 && safe_writes == n + 1 && get_long("head/safe") == 1,
              "an expected reap (the stop lever's stop) makes the head safe too");
        /* A head that did not probe has no attribute: said below the
         * safing writes' severity, and the reap goes on. */
        unlink(GF_SYSFS "head/safe");
        super_controller_start();
        CHECK(run_until_spawned(5.0), "the controller comes back");
        int c = crit_logs;
        die(0x0900);
        tick();
        CHECK(child_pid == 0 && crit_logs == c, "no head/safe: the reap names no safing failure");
        put("head/safe", "0\n");
    }
    reset_state();
    {
        /* The supervisor's spawn, from a thread of its own that then ends,
         * as the lifecycle thread ends with the daemon; the controller is
         * a real child that waits at its exec. */
        pthread_t th_spawn;
        int status = 0;
        real_fork_controller = 1;
        pthread_create(&th_spawn, NULL, spawn_from_thread, NULL);
        pthread_join(th_spawn, NULL);
        pid_t ctl = child_pid;
        if (ctl > 0)
            sigqueue(ctl, SIGCONT, (union sigval){ 0 });
        pid_t w = ctl > 0 ? wait4(ctl, &status, 0, NULL) : -1;
        CHECK(w == ctl && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
              "the controller is killed (SIGKILL) when the thread that forked it ends");
        pthread_mutex_lock(&mu);
        child_pid = 0;
        child_ctl = Ctl_None;
        pthread_mutex_unlock(&mu);
        /* A child whose daemon is gone before it armed the signal has
         * another parent: it does not run. */
        pid_t c = fork();
        if (c == 0)
            _exit(ctl_dies_with_daemon(getppid()) == 0 && ctl_dies_with_daemon(getppid() + 1) == -1 ? 0 : 1);
        status = 0;
        w = c > 0 ? wait4(c, &status, 0, NULL) : -1;
        CHECK(w == c && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "its own daemon as its parent: armed; another parent: refused");
        /* The spawn's child whose parent check fails runs nothing: it exits
         * 125 before its exec. */
        real_fork_controller = 1;
        ppid_lie = 1;
        pthread_mutex_lock(&mu);
        spawn_locked(Ctl_Cloud);
        pid_t refused = child_pid;
        child_pid = 0;
        child_ctl = Ctl_None;
        pthread_mutex_unlock(&mu);
        ppid_lie = 0;
        status = 0;
        w = refused > 0 ? wait4(refused, &status, WUNTRACED, NULL) : -1;
        if (w == refused && WIFSTOPPED(status)) {
            sigqueue(refused, SIGKILL, (union sigval){ 0 });
            wait4(refused, NULL, 0, NULL);
        }
        CHECK(w == refused && WIFEXITED(status) && WEXITSTATUS(status) == 125,
              "the spawn's child exits 125 before its exec when its parent check fails");
    }
    reset_state();
    {
        /* A takeover: the head made safe as the unmanaged controller is
         * ended, before the motion probe, the lens sweep and the fork. */
        int n = safe_writes;
        put("cnc/state", "idle\n");
        put("head/safe", "0\n");
        calls[0] = '\0';
        pthread_mutex_lock(&mu);
        head_safed = 0;                 /* a daemon that stood by at its start */
        standby_takeover = 1;
        suspended = SUSP_STANDBY;
        pthread_mutex_unlock(&mu);
        CHECK(run_until_spawned(5.0) && strstr(calls, "/etc/init.d/grblhal stop") && safe_writes == n + 1 &&
              safe_probes == 0 && safe_forks == 0 && get_long("head/safe") == 1,
              "a takeover makes the head safe as it ends the unmanaged controller, before the probe and the "
              "fork, and the spawn writes it no second time");
    }
    reset_state();
    {
        /* A mode switch over a standby whose unmanaged controller ended by
         * itself: the switch makes the head safe, before the probe. */
        int n;
        put("cnc/state", "idle\n");
        runner_alive = 1;
        pthread_mutex_lock(&mu);
        head_safed = 0;
        init_locked();
        pthread_mutex_unlock(&mu);
        runner_alive = 0;               /* it ended on its own */
        n = safe_writes;
        switch_takeover();
        pthread_mutex_lock(&mu);        /* the switch's own state change */
        want = Ctl_Grbl;
        suspended = 0;
        standby_takeover = 0;
        pthread_mutex_unlock(&mu);
        CHECK(run_until_spawned(5.0) && safe_writes == n + 1 && safe_probes == 0 && safe_forks == 0,
              "a mode switch over a standby whose controller ended makes the head safe once, before the probe");
    }
    reset_state();
    {
        /* A standby beside a live unmanaged controller, a
         * POST /controller/start that lifts the suspension, then a mode
         * switch while that controller still runs: taken over, not made
         * safe under it. */
        put("cnc/state", "idle\n");
        runner_alive = 1;
        pthread_mutex_lock(&mu);
        head_safed = 0;
        init_locked();
        pthread_mutex_unlock(&mu);
        super_controller_start();
        calls[0] = '\0';
        int n = safe_writes;
        switch_takeover();
        CHECK(strstr(calls, "/etc/init.d/grblhal stop") != NULL && safe_writes == n + 1,
              "a mode switch after /controller/start over a live unmanaged controller takes it over");
    }
    reset_state();
    {
        /* An operator stop (the stop lever), no standby, a controller
         * started by hand, then a mode switch: taken over, head/safe once. */
        put("cnc/state", "idle\n");
        pthread_mutex_lock(&mu);
        suspended = SUSP_HOLD;
        standby_takeover = 0;
        pthread_mutex_unlock(&mu);
        runner_alive = 1;
        calls[0] = '\0';
        int n = safe_writes;
        switch_takeover();
        CHECK(strstr(calls, "/etc/init.d/grblhal stop") != NULL && safe_writes == n + 1,
              "a mode switch over the stop lever's suspension takes a live unmanaged controller over");
        runner_alive = 0;
    }
    reset_state();
    {
        /* The daemon's start: super_init makes the head safe before its
         * lifecycle thread runs. */
        int n;
        pthread_mutex_lock(&mu);
        head_safed = 0;
        pthread_mutex_unlock(&mu);
        n = safe_writes;
        super_init();
        int at_start = safe_writes - n;
        super_shutdown();
        CHECK(at_start == 1 && th_run == 0, "super_init makes the head safe, then its lifecycle thread runs and stops");
    }
    reset_state();

    printf(failures ? "super_test: %d FAILED\n" : "super_test: all ok\n", failures);
    return failures ? 1 : 0;
}
