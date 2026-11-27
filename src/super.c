/*
 * super.c - forgectrl: controller-mode supervisor
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * forgectrl owns the lifecycle of the motion controllers: exactly one
 * of grblHAL (GRBL mode) or gfcloud (Glowforge web-service mode) runs
 * at a time, spawned as a direct child of this daemon. The parent-child
 * relationship is load-bearing: it is how the pulse-device broker hands
 * the controller its fd at spawn, and how a controller death is
 * detected the moment it happens: SIGCHLD wakes the lifecycle thread,
 * which safes the machine within milliseconds of the exit.
 *
 * One thread owns the whole lifecycle (spawn, reap, respawn, stop) and
 * everything else talks to it through a small request mailbox - there
 * is exactly one waitpid() caller for the controller pid. Every
 * transition out of a running child - expected or not, including
 * SIGKILL escalation - safes the machine (cnc/stop + laser latch):
 * under the broker a child exit is not a final close of the pulse
 * device, so these writes are the safing mechanism. Unexpected deaths
 * additionally respawn with backoff, once the homing runner the
 * controller may have left behind is gone and the kernel has finished
 * what it was playing. The cooling engine's fail tiers (a fire signal,
 * a head crash) end the controller through the same stop path, and the
 * loop respawns it.
 *
 * The enclosure check (the lid and the interlock closed, the switches
 * readable) runs before every spawn, respawns included; the motion
 * probe runs once per broker hold. The lens reference marker goes at
 * every controller exit, and the lens is swept onto its hall edge again
 * before the next GRBL controller starts.
 *
 * The mode-switch sequence (POST /mode) is idle-gated: stop the active
 * controller, persist controller_mode, start the other, wait for its
 * first job-state report to reach the cooling engine. Diagnostics use
 * the same machinery to take the hardware: suspend (controller down,
 * mode unchanged) and resume - the controller that comes back is the
 * selected mode's, whichever that is.
 *
 * The boot-time init scripts do not start controllers; they defer here.
 * If an unmanaged controller is found running at startup anyway (legacy
 * scripts, a manual start, or a homing runner a crashed daemon's GRBL
 * controller left behind), the supervisor stands by rather than fight
 * over the exclusive-open pulse device and the Grbl port.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "cam.h"
#include "setup.h"
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "lease.h"
#include "led.h"
#include "lenshome.h"
#include "liveness.h"
#include "paths.h"
#include "settings.h"
#include "status.h"
#include "stopsig.h"
#include "super.h"
#include "update.h"
#include "testhook.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GRBL_BIN   "/usr/bin/grblHAL_glowforge"
#define CLOUD_BIN  "/usr/sbin/gfcloud.py"
/* The GRBL controller's settings store (the $-settings), inside the
 * data directory. */
#define GRBL_NVS_FILE "EEPROM-glowforge.DAT"
/* The sysfs tree and the pulse device: the host unit test points both
 * at a directory of plain files. At run time the test hooks GF_SYSFS_ROOT
 * and GF_PULSE_DEV move the tree and the device in a host-test run
 * (testhook.h), and FORGECTRL_GRBL_BIN and FORGECTRL_CLOUD_BIN override
 * the two controllers; unset, each is the path below. GF_PULSE_DEV and
 * the two binaries must be absolute paths, or they are refused. */
#ifndef GF_SYSFS
#define GF_SYSFS "/sys/glowforge/"
#endif
#ifndef PULSE_DEV
#define PULSE_DEV  "/dev/glowforge"
#endif
#define HOMED_ANCHOR "/run/grblhal.homed"

#define STOP_TERM_WAIT_S   5      /* SIGTERM grace before SIGKILL */
#define RESPAWN_MIN_S      1
#define RESPAWN_MAX_S      30
#define HEALTHY_UPTIME_S   60     /* uptime that resets the backoff */
#define REPORT_WAIT_S      15     /* mode switch: first /cool/state */
#define LOOP_TICK_MS       200    /* the lifecycle pass, when nothing wakes it */
#define RUNNER_TERM_WAIT_S 5      /* the homing runner's SIGTERM grace */
#define RESPAWN_IDLE_WAIT_S 10    /* a respawn waits this long for the kernel */
#define BROKER_RETRY_S     2      /* the broker open is tried again after this */
#define PROBE_BUSY_WAIT_S  10     /* the probe waits this long for a kernel still playing */
#define WR_ATTR_TRIES      3      /* a safing write is tried this often... */
#define WR_ATTR_RETRY_US   10000  /* ...this far apart */
#define LEVER_WAIT_S       15     /* the stop lever's and a takeover's wait */
/* The daemon's stop waits this long for the lifecycle thread to end the
 * controller (stop_locked: its SIGTERM grace, then SIGKILL and as long
 * again) and a probe or a sweep in flight, before it stops the controller
 * from its own thread. */
#define SHUTDOWN_WAIT_S    (2 * STOP_TERM_WAIT_S + 5)

typedef enum { Ctl_None = 0, Ctl_Grbl, Ctl_Cloud } ctl_t;

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v && *v) ? v : def;
}

/* GF_SYSFS_ROOT ends with '/', as status.c reads it. */
static const char *sysfs_root(void)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    return r ? r : GF_SYSFS;
}

/* The pulse device: the test hook GF_PULSE_DEV in a host-test run
 * (testhook.h) when it is an absolute path; NULL when it is set there and
 * is not, which the broker refuses (a relative path would be read against
 * whatever directory the daemon was started in); else PULSE_DEV. */
static const char *pulse_dev(void)
{
    const char *v = ff_test_hook("GF_PULSE_DEV");
    if (v)
        return v[0] == '/' ? v : NULL;
    return PULSE_DEV;
}

/* The controller's binary: FORGECTRL_GRBL_BIN and FORGECTRL_CLOUD_BIN
 * override the two when they are set and absolute paths; NULL when one is
 * set and is not, which the spawn refuses (a relative path would be found
 * against whatever directory and search path the daemon started with). */
static const char *ctl_bin(ctl_t ctl)
{
    const char *v = ctl == Ctl_Grbl ? env_or("FORGECTRL_GRBL_BIN", GRBL_BIN)
                                    : env_or("FORGECTRL_CLOUD_BIN", CLOUD_BIN);
    return v[0] == '/' ? v : NULL;
}

/* In a host-test run (FORGECTRL_HOST_TEST=1, testhook.h) no process the
 * supervisor starts writes its OOM score. */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t th;
static int th_run = 0;

static ctl_t want = Ctl_None;      /* selected mode */
/* Why supervision is suspended, one bit per reason: each is set and
 * cleared by its own owner, and a controller runs only with none. A
 * setup check's or a diagnostic's hand-back clears its takeover and
 * nothing else, so an operator's stop made meanwhile stands. */
#define SUSP_HOLD     1u           /* the stop lever (POST /controller/stop) */
#define SUSP_TAKEOVER 2u           /* a setup check or a diagnostic has the machine */
#define SUSP_STANDBY  4u           /* an unmanaged controller */
#define SUSP_SHUTDOWN 8u           /* the daemon's stop: nothing clears it */
static unsigned suspended = 0;
static pid_t child_pid = 0;
static pid_t stop_pid = 0;         /* the controller a deliberate stop ends */
static ctl_t child_ctl = Ctl_None;
static double spawned_at = 0.0;
static unsigned backoff_s = RESPAWN_MIN_S;
static double respawn_at = 0.0;    /* not before this time */
static unsigned generation = 0;    /* bumped on every state change */

static int local_posture = 0;      /* a wizard owns the controller: loopback only */
static int gated = 0;              /* the setup gate is closed (under mu) */
static char gate_why[256];

/* A dev image booted from an SD card whose data partition is there and not
 * in use runs on the eMMC's settings, read-only, its logs in RAM
 * (forgefirm-data): no controller, so nothing runs on a setup that is not
 * the card's. The boot's one word, as the dev bar reads it: emmc, card and
 * card-none each say where this boot's settings are. Any other word holds
 * the controllers, and so does a record that is missing or cannot be read:
 * a dev image that cannot tell whose settings it runs on starts nothing on
 * them. The card's branch is the dev image's alone, so a release image is
 * never held here. */
static int card_data_failed(void)
{
    if (!auth_dev_image())
        return 0;
    const char *hook = ff_test_hook("GF_DATA_STATE");
    char w[32] = "";
    FILE *f = fopen(hook ? hook : "/run/forgefirm-data.state", "r");
    if (f) {
        if (!fgets(w, sizeof(w), f))
            w[0] = '\0';
        fclose(f);
    }
    w[strcspn(w, "\r\n")] = '\0';
    return strcmp(w, "emmc") && strcmp(w, "card") && strcmp(w, "card-none");
}

/* The controller gate: the setup's, and the card's data partition. */
static int controller_gate_open(char *why, size_t len)
{
    if (card_data_failed()) {
        snprintf(why, len, "the SD card's data partition is not in use: nothing is saved this boot "
                 "(see the boot log)");
        return 0;
    }
    return setup_gate_open(why, len);
}

static int broker_fd = -1;         /* /dev/glowforge, held for our lifetime */
static int broker_warned = 0;      /* the broker's refusal is said once per episode */
static int probed = 0;             /* liveness gate passed since broker open */
static int probe_skipped = 0;      /* ...but the probe itself could not run */
static int motion_fault = 0;       /* probe failed after recovery - no spawn */
static int sweep_failed = 0;       /* a lens sweep found no edge in this hold: none again */
static int probing = 0;            /* a probe sequence is in flight (under mu) */
static int probe_abort = 0;        /* ...and the stop lever asked it to end */
static int standby_takeover = 0;   /* unmanaged controller found: retake at idle */
static int enclosure_wait = 0;     /* the gate waits for the lid or the interlock */
static char wait_why[96];          /* ...and this is what is open */
static int runner_check = 0;       /* a GRBL reap: look for its homing runner */
static int respawn_wait_idle = 0;  /* the respawn waits for the kernel to finish */
static double died_at = 0.0;       /* ...counted from this unexpected death */
static int restart_pending = 0;    /* the engine asked for a stop and a respawn */
/* The daemon's HTTP listener is not up yet (super_init to super_listening):
 * nothing moves and no controller starts before it is, so a daemon that
 * cannot start its listener, and leaves through its stop, has moved
 * nothing. Under mu. */
static int await_listener = 0;
static int chld_efd = -1;          /* SIGCHLD lands here; the loop waits on it */

static int wr_attr(const char *attr, const char *val);
static int wr_enable_once(void);

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One kernel attribute's word, trailing white space dropped: 0, or -1
 * (buf empty) when it cannot be read. */
static int rd_attr(const char *attr, char *buf, size_t len)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) {
        buf[0] = '\0';
        return -1;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

/* The kernel is playing a program, so stopping its controller now would
 * cut it. A state that cannot be read counts as playing: the callers
 * then leave the machine as it is. fault, underrun and disabled play
 * nothing. */
static int kernel_playing(void)
{
    char st[24];
    return rd_attr("cnc/state", st, sizeof(st)) != 0 || strcmp(st, "running") == 0;
}

static int head_safe_write(void);
static int head_safed;
static void safing_pair(void);

/* The cloud client as the supervisor starts it, for pgrep and pkill -f: the
 * interpreter its first line names, then the script, then nothing or its
 * arguments, from the command line's start (never a substring of another
 * command line, an editor's or a test run's). Quoted for the shell. */
#define CLOUD_CLIENT_RE "'^/usr/bin/python3 " CLOUD_BIN "( |$)'"

/* Stop an unmanaged controller (one a legacy script or a hand started, or
 * a GRBL controller's homing runner that outlived its controller and the
 * daemon) by its signals so the supervisor can take over, and make the
 * head safe after it, as a reap does: whatever it left on the head goes
 * off before the motion probe and the lens sweep that come next. Called
 * with mu NOT held. */
static void takeover_unmanaged(void)
{
    fflog(LOG_WARNING, "super: taking over from unmanaged "
                       "controller");
    /* The safing pair before the signals and after them, as every stop of
     * a controller writes it. */
    safing_pair();
    (void)!system("pkill -x grblHAL_glowfor 2>/dev/null");
    (void)!system("pkill -f " CLOUD_CLIENT_RE " 2>/dev/null");
    (void)!system("pkill -x gfhome.py 2>/dev/null");
    sleep(1);
    safing_pair();
    pthread_mutex_lock(&mu);
    head_safe_write();
    head_safed = 1;
    pthread_mutex_unlock(&mu);
}

/* The lifecycle thread's wake: SIGCHLD (a controller exit) and the
 * requests other threads post. The handler may only write. */
static void on_sigchld(int sig)
{
    (void)sig;
    uint64_t one = 1;
    if (chld_efd >= 0)
        (void)!write(chld_efd, &one, sizeof(one));
}

static void wake(void)
{
    on_sigchld(0);
}

/* One pass's wait: the tick, or sooner when a child exits or a
 * request lands. */
static void loop_wait(int ms)
{
    if (chld_efd < 0) {
        usleep((useconds_t)ms * 1000);
        return;
    }
    struct pollfd p = { .fd = chld_efd, .events = POLLIN };
    if (poll(&p, 1, ms) > 0) {
        uint64_t n;
        (void)!read(chld_efd, &n, sizeof(n));
    }
}

/* The GRBL controller's homing runner (gfhome.py) runs in its own
 * process group and inherits the pulse fd: a controller that dies
 * during $H leaves it alive, with the ring writable. It is gone before
 * anything else may write the ring. Called with mu NOT held. */
static int runner_running(void)
{
    return system("pgrep -x gfhome.py >/dev/null 2>&1") == 0;
}

static void runner_reap(void)
{
    if (!runner_running())
        return;
    fflog(LOG_WARNING, "super: the homing runner outlived its controller - "
                       "stopping it");
    /* The safing pair before each signal and after the runner's end: a
     * homing move it queued stops, whatever lifecycle state follows (no
     * respawn may come to halt it). */
    safing_pair();
    (void)!system("pkill -TERM -x gfhome.py 2>/dev/null");
    double deadline = wall_s() + RUNNER_TERM_WAIT_S;
    while (runner_running() && wall_s() < deadline)
        usleep(100 * 1000);
    if (runner_running()) {
        fflog(LOG_WARNING, "super: escalating the homing runner to SIGKILL");
        safing_pair();
        (void)!system("pkill -KILL -x gfhome.py 2>/dev/null");
        deadline = wall_s() + 1.0;
        while (runner_running() && wall_s() < deadline)
            usleep(100 * 1000);
    }
    safing_pair();
}

/* The pulse-device broker: one open for the daemon's lifetime, handed
 * to every controller as an inherited fd (GF_PULSE_FD). The device is
 * exclusive-open, so holding it also *enforces* that no one else can
 * open it; the flock arms the kernel dead-man on the shared
 * description (final close mid-run = e-stop - i.e. forgectrl dying
 * with no writer alive). Opened lazily at the first managed spawn so
 * standby next to a legacy self-opening controller stays conflict-free.
 * Called with mu held. */
static void broker_open_locked(void)
{
    if (broker_fd >= 0)
        return;
    const char *dev = pulse_dev();
    if (!dev) {
        if (!broker_warned)
            fflog(LOG_CRIT, "super: GF_PULSE_DEV is not an absolute path - no "
                            "controller until it is");
        broker_warned = 1;
        return;
    }
    /* A just-stopped legacy holder's close can lag its exit. O_CLOEXEC:
     * only the controller spawn clears the flag before exec, so helper
     * children (curl, fwup, media-ctl, pkill, ...) can never pin the
     * pulse device and defeat the kernel dead-man on the description. */
    for (int i = 0; i < 30 && broker_fd < 0; i++) {
        broker_fd = open(dev, O_WRONLY | O_CLOEXEC);
        if (broker_fd < 0)
            usleep(100 * 1000);
    }
    /* No broker, no controller: a controller that opens the device
     * itself settles the rail on its own open (the one path that cycles
     * the 40 V), and a description without the lock has no dead-man.
     * The loop tries again; the refusal is said once per episode. */
    if (broker_fd < 0) {
        if (!broker_warned)
            fflog(LOG_CRIT, "super: cannot open %s: %s - no "
                            "controller until the broker holds it",
                  dev, strerror(errno));
        broker_warned = 1;
        return;
    }
    if (flock(broker_fd, LOCK_EX) != 0) {
        if (!broker_warned)
            fflog(LOG_CRIT, "super: flock on %s failed: %s - the "
                            "kernel dead-man would be unarmed; no controller "
                            "until it locks", dev, strerror(errno));
        broker_warned = 1;
        close(broker_fd);
        broker_fd = -1;
        return;
    }
    broker_warned = 0;
    probed = 0;     /* a fresh hold means unverified motion */
    probe_skipped = 0;
    sweep_failed = 0;
    lenshome_clear();   /* ...and an unreferenced lens */
    fflog(LOG_INFO, "super: holding %s (broker)", dev);
}

/* The gate's wait for the enclosure. The probe moves the gantry, so it
 * does not run while a lid or the interlock is open, or while the
 * switches cannot be read: no controller until the enclosure closes,
 * /mode says "waiting" and why, and the button blinks amber so the
 * machine itself asks for the lid. The gate comes here on every pass of
 * the wait, and each one keeps the amber: the cooling engine's red flash
 * stands over it and leaves the button dark when it ends. Called with mu
 * held. */
static void wait_enter_locked(const char *why)
{
    if (enclosure_wait && strcmp(why, wait_why) == 0) {
        led_keep(LED_BLINK_AMBER);
        return;
    }
    fflog(LOG_WARNING, "super: %s - the motion check waits for it to "
                       "close; no controller until then", why);
    snprintf(wait_why, sizeof(wait_why), "%s", why);
    enclosure_wait = 1;
    generation++;
    led_set(LED_BLINK_AMBER);
}

static void wait_leave_locked(const char *how)
{
    if (!enclosure_wait)
        return;
    fflog(LOG_NOTICE, "super: motion check %s", how);
    enclosure_wait = 0;
    wait_why[0] = '\0';
    generation++;
    led_release();
}

int super_motion_fault(void)
{
    pthread_mutex_lock(&mu);
    int fault = motion_fault;
    pthread_mutex_unlock(&mu);
    return fault;
}

/* The stop lever's reach into a probe in flight: the ladder looks
 * between its steps, so a stop lands within a step's wait. Called with
 * mu NOT held; 1 when the probe must end. */
static int probe_aborted(void)
{
    pthread_mutex_lock(&mu);
    int a = probe_abort;
    pthread_mutex_unlock(&mu);
    return a;
}

static int probe_wait(unsigned s)
{
    double until = wall_s() + s;
    while (wall_s() < until) {
        if (probe_aborted())
            return 1;
        usleep(100 * 1000);
    }
    return 0;
}

/* The lens hunts' stop inputs, asked between two steps (mu NOT held). A
 * lid or the interlock that opens, or switches that stop answering, end
 * the hunt as the lever does, and hunt_lid says it was the enclosure. The
 * lever is probe_abort: the stop lever, a check's takeover and the
 * daemon's shutdown each set it when they find a probe or a sweep in
 * flight (probing), and one that lands before either begins finds the
 * suspension it set refusing it. */
static int hunt_lid;

static int enclosure_opened(void)
{
    char why[48];
    if (liveness_enclosure(why, sizeof(why)) == 0)
        return 0;
    hunt_lid = 1;
    return 1;
}

static int probe_hunt_stop(void)
{
    return probe_aborted() || enclosure_opened();
}

static int sweep_stop(void)
{
    return probe_aborted() || enclosure_opened();
}

/* Motion-liveness gate, run with mu NOT held (takes seconds; the
 * recovery ladder takes a minute). The DRV8825 drivers can come out of
 * a rail power-up unserviceable; each recovery attempt gives them a
 * longer true power-off before re-probing. Returns 1 verified, 0 fault
 * (dead drivers after the ladder, a stepper fault a dead verdict finds,
 * which no rung cycles, a lens with no edge, a stepper enable the kernel
 * refuses, or a probe the machine fails, each with its own
 * words), 2 when the probe could not run (no accelerometer): the machine
 * is not blocked, but motion stays UNVERIFIED and is reported so; 3 when
 * the probe must wait and run again - the enclosure opened between the
 * gate's own check and the move, under the move, or before or during the
 * lens hunt, or the kernel is still playing the last program: nothing is
 * decided (a third move stopped under an opening with no whole move
 * between is a fault); 4 when the stop lever ended it, the move unstarted
 * when it landed before the run. The fans are not quieted for
 * the probe: its verdict is peak-to-peak over a commanded move, and the
 * moving and dead thresholds sit twice away from what the bench
 * reference reads with its fans at their idle duty. */
static char probe_detail[96];       /* the last probe's outcome text */

/* The X and Y motors are released ($MD in the GRBL controller): their
 * step currents are at 0 and an operator's hands may be on the gantry.
 * The release outlives its controller as a marker in the run directory,
 * and nothing that starts here may energize X or Y under it. The two
 * things that would are the probe, which is skipped (motion stays
 * unverified; the controller still starts, and takes the release over),
 * and a switch to cloud mode, whose homing moves the head, which is
 * refused. The lens motor is not released: it stays at its hold
 * current, and the lens sweep before a GRBL controller
 * (lens_sweep_locked) still runs under the marker. It steps Z alone,
 * behind the enclosure check every spawn passes. */
static int motors_released(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/motors.released", ff_run_dir());
    return access(path, F_OK) == 0;
}

/* Moves an opening enclosure stopped since the last move that ran whole.
 * Each one leaves the head where the stop caught it, toward +X of its
 * start, and the next probe goes +X first from there; a probe that moved
 * nothing (the enclosure open before its move, a busy kernel, the lever)
 * brings the head no nearer its start, so only a whole move ends the
 * count. The count bounds the walk to PROBE_STOPPED_MAX out legs. */
#define PROBE_STOPPED_MAX 3
static int moves_stopped;

static int probe_sequence(int fd)
{
    static const int ladder_s[] = { 0, 5, 15, 30 };
    char *detail = probe_detail;

    if (motors_released()) {
        /* Nothing is written to the rail. A kernel left disabled (a module
         * load, a drill's disable) stays disabled, and the controller's
         * runs are refused in it until a probe powers the rail and verifies
         * the gantry after it: the Motion check, once $ME ends the
         * release. The log says so. */
        char st[24];
        if (rd_attr("cnc/state", st, sizeof(st)) == 0 && !strcmp(st, "disabled"))
            fflog(LOG_WARNING, "super: the kernel reads disabled under the motor release - the "
                               "controller's moves are refused until the Motion check, after $ME");
        snprintf(detail, sizeof(probe_detail), "the X and Y motors are released: nothing was moved");
        fflog(LOG_NOTICE, "super: liveness probe: SKIPPED - %s", detail);
        return 2;
    }

    /* The kernel as the last holder left it. A program still playing is
     * waited for, bounded, and then halted, as a respawn does. An
     * underrun is acknowledged: the enable below is refused in it. A
     * latched stepper fault is cleared by the enable below once its lines
     * have cleared, and the mask is said either way. */
    char st[24], faults[16] = "";
    rd_attr("cnc/state", st, sizeof(st));
    if (!strcmp(st, "running")) {
        double until = wall_s() + PROBE_BUSY_WAIT_S;
        while (rd_attr("cnc/state", st, sizeof(st)) == 0 && !strcmp(st, "running")) {
            if (wall_s() >= until) {
                fflog(LOG_CRIT, "super: the kernel is still running %d s before the "
                                "motion probe - halting it", PROBE_BUSY_WAIT_S);
                wr_attr("cnc/halt", "1");
                break;
            }
            if (probe_wait(1))
                goto aborted;
        }
    }
    if (!strcmp(st, "underrun")) {
        fflog(LOG_NOTICE, "super: the kernel reads underrun - acknowledged before the motion probe");
        wr_attr("cnc/stop", "1");
    }
    int cleared = 0;
    if (!strcmp(st, "fault")) {
        rd_attr("cnc/faults", faults, sizeof(faults));
        fflog(LOG_WARNING, "super: the kernel holds a stepper fault (faults %s) - the probe's "
                           "enable clears it once its lines have cleared", faults[0] ? faults : "unreadable");
        cleared = 1;
    }

    for (size_t i = 0; i < sizeof(ladder_s) / sizeof(ladder_s[0]); i++) {
        if (ladder_s[i] > 0) {
            /* The ladder is for drivers that stopped with no fault
             * flagged. A stepper fault (the probe's own move can raise
             * one) is never cycled away: the kernel's disable leaves a
             * fault as it is, and the probe ends here as a motion fault
             * naming the mask, with no rail-off and no enable. A state
             * that cannot be read ends it the same way. */
            if (rd_attr("cnc/state", st, sizeof(st)) != 0 || !strcmp(st, "fault")) {
                rd_attr("cnc/faults", faults, sizeof(faults));
                if (st[0])
                    snprintf(detail, sizeof(probe_detail), "no motion, and a stepper fault "
                             "(faults %s): no rail cycle", faults[0] ? faults : "unreadable");
                else
                    snprintf(detail, sizeof(probe_detail), "no motion, and the kernel's state "
                             "cannot be read: no rail cycle");
                fflog(LOG_CRIT, "super: MOTION FAULT - %s. Controllers stay down (Retry, or the "
                                "Motion check, once the drivers' fault lines clear).", detail);
                return 0;
            }
            fflog(LOG_WARNING, "super: motion dead - rail off %d s "
                               "and re-probing (%zu/3)", ladder_s[i], i);
            wr_attr("cnc/disable", "1");
            if (probe_wait((unsigned)ladder_s[i]))
                goto aborted;
        }
        if (probe_aborted())
            goto aborted;
        if (wr_enable_once() != 0) {
            /* One enable, written once, never a loop: a kernel that
             * refuses it (a stepper fault whose lines still read asserted,
             * or a fault latched while the rail was off) keeps the
             * controllers down, in its own words, until the operator
             * retries. */
            rd_attr("cnc/state", st, sizeof(st));
            rd_attr("cnc/faults", faults, sizeof(faults));
            snprintf(detail, sizeof(probe_detail), "the kernel refused the stepper enable "
                     "(state %s, faults %s)", st[0] ? st : "unreadable", faults[0] ? faults : "unreadable");
            fflog(LOG_CRIT, "super: MOTION FAULT - %s. Controllers stay down (Retry, or the "
                            "Motion check, once the drivers' fault lines clear).", detail);
            return 0;
        }
        if (cleared) {
            fflog(LOG_WARNING, "super: the stepper fault is cleared (faults %s)",
                  faults[0] ? faults : "unreadable");
            cleared = 0;
        }
        if (probe_wait(1))
            goto aborted;
        led_set(LED_SOLID_WHITE);       /* the machine is moving */
        int rc = liveness_probe(fd, detail, sizeof(probe_detail), probe_aborted);
        led_release();
        fflog(rc == -3 ? LOG_NOTICE : LOG_INFO, "super: liveness probe: %s - %s",
              rc == 1 ? "MOTION OK" : rc == 0 ? "NO MOTION"
              : rc == -3 ? "WAITS" : "ERROR", detail);
        /* A lever that landed while the probe ran, before its move (which
         * then never started) or under it: what it stopped says nothing
         * about the drivers, and the lens stays where it is. */
        if (probe_aborted() || rc == -6)
            goto aborted;
        if (rc == -5) {
            /* The move was stopped under an opening: wait for the
             * enclosure and probe again, but not without bound. */
            if (++moves_stopped < PROBE_STOPPED_MAX)
                return 3;
            moves_stopped = 0;
            snprintf(detail, sizeof(probe_detail), "the enclosure opened under the probe's move %d "
                     "times", PROBE_STOPPED_MAX);
            fflog(LOG_CRIT, "super: MOTION FAULT - %s with no whole move between: the head stands "
                            "off its start. Controllers stay down (retry via POST /mode).", detail);
            return 0;
        }
        if (rc == 0 || rc == 1)
            moves_stopped = 0;  /* a move that ran whole, out and back */
        if (rc == -3)
            return 3;   /* the kernel is busy: wait for it, then probe */
        if (rc == -4) {
            /* The probe failed on the machine (a write the kernel or the
             * PIC refused, the run, the samples): no rail cycle would
             * mend that, and a live gantry would read as dead. */
            fflog(LOG_CRIT, "super: MOTION FAULT - the motion probe failed: %s. "
                            "Controllers stay down (retry via POST /mode).", detail);
            return 0;
        }
        if (rc == 1) {
            /* The gantry moves; now the lens takes its one reference,
             * while the machine is still ours. A lens that cannot reach
             * its hall edge is broken hardware, so it gates the spawn the
             * same way dead drivers do: the focal height would otherwise
             * be a guess. The lens does not move with a lid or the
             * interlock open: the probe waits and runs again. */
            char why[48];
            if (liveness_enclosure(why, sizeof(why)) != 0) {
                snprintf(detail, sizeof(probe_detail), "%s - the lens waits for it", why);
                fflog(LOG_NOTICE, "super: %s", detail);
                return 3;
            }
            /* Sized to leave room for the prefix below inside
             * probe_detail, which this shares with the probe. */
            char lens[80];
            hunt_lid = 0;
            int lrc = lenshome_run(lens, sizeof(lens), probe_hunt_stop);
            if (lrc == LENSHOME_STOPPED) {
                if (!hunt_lid)
                    goto aborted;
                snprintf(detail, sizeof(probe_detail), "the enclosure opened during the lens "
                         "reference - the probe waits for it");
                fflog(LOG_NOTICE, "super: %s", detail);
                return 3;
            }
            fflog(lrc == 0 ? LOG_CRIT : LOG_INFO,
                  "super: lens reference: %s - %s",
                  lrc == 1 ? "ON EDGE" : lrc == 0 ? "NO EDGE" : "SKIPPED", lens);
            if (lrc == 0) {
                snprintf(detail, sizeof(probe_detail), "lens: %s", lens);
                return 0;
            }
            return 1;
        }
        if (rc == -2)
            return 3;   /* the enclosure open before the move: wait for it */
        if (rc < 0)
            return 2;   /* no head accelerometer: do not block the machine */
    }
    fflog(LOG_CRIT, "super: MOTION FAULT - the stepper drivers "
                   "did not recover; a full power cycle may be required. "
                   "Controllers stay down (retry via POST /mode).");
    return 0;

aborted:
    /* The lever ended the probe: whatever the rung had started stops,
     * and nothing is decided about motion. */
    wr_attr("cnc/stop", "1");
    snprintf(detail, sizeof(probe_detail), "motion probe ended by the stop lever");
    fflog(LOG_NOTICE, "super: %s", detail);
    return 4;
}

/* One write of val to a kernel attribute: 0 when it landed, else its
 * errno. */
static int wr_attr_try(const char *attr, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    ssize_t w = write(fd, val, strlen(val));
    int err = w < 0 ? errno : 0;
    close(fd);
    return err;
}

/* The safing writes (cnc/stop, cnc/laser_latch) are the real safing
 * mechanism under the device broker: a write that fails is tried again
 * before it is named at the highest severity, and the callers on the
 * safing paths repeat the pair once more. Returns 0 when the value
 * landed. */
static int wr_attr(const char *attr, const char *val)
{
    int err = 0;
    for (int i = 0; i < WR_ATTR_TRIES; i++) {
        if (i)
            usleep(WR_ATTR_RETRY_US);
        err = wr_attr_try(attr, val);
        if (!err)
            return 0;
    }
    fflog(LOG_CRIT, "super: writing %s to %s failed %d times: %s", val, attr,
          WR_ATTR_TRIES, strerror(err));
    return -1;
}

/* The safing pair: the latch lock first, which severs FIRE whatever state
 * the kernel is in, then the stop. The kernel refuses a stop in its fault
 * state (EPERM), where nothing plays and FIRE is parked already: that
 * refusal is nothing to stop, said below the safing writes' severity and
 * not tried again. Any other failure is wr_attr's: tried again, then named
 * at the highest severity. */
static void safing_stop(void)
{
    int err = 0;
    for (int i = 0; i < WR_ATTR_TRIES; i++) {
        if (i)
            usleep(WR_ATTR_RETRY_US);
        err = wr_attr_try("cnc/stop", "1");
        if (!err)
            return;
        char st[24];
        if (err == EPERM && rd_attr("cnc/state", st, sizeof(st)) == 0 && !strcmp(st, "fault")) {
            fflog(LOG_INFO, "super: the kernel is in its fault state: no stop to make");
            return;
        }
    }
    fflog(LOG_CRIT, "super: writing 1 to cnc/stop failed %d times: %s", WR_ATTR_TRIES,
          strerror(err));
}

static void safing_pair(void)
{
    wr_attr("cnc/laser_latch", "1");
    safing_stop();
}

/* The stepper enable is no safing write, and it is written once: its
 * refusal is the answer. An enable the kernel refuses for a fault that
 * lands in its own power-on has already taken the 40 V rail on and off
 * again, and each further try would be another rail cycle over a faulting
 * driver, which nothing here does to recover. Returns 0 when the kernel
 * took it. */
static int wr_enable_once(void)
{
    int err = wr_attr_try("cnc/enable", "1");
    if (!err)
        return 0;
    fflog(LOG_CRIT, "super: writing 1 to cnc/enable failed: %s", strerror(err));
    return -1;
}

static const char *ctl_name(ctl_t c)
{
    return c == Ctl_Grbl ? "grbl" : c == Ctl_Cloud ? "cloud" : "none";
}

/* ------------------------------------------------------------- spawn */

extern char **environ;

/* The controller's environment, assembled in the parent: the daemon is
 * multithreaded, so between fork() and exec the child may only call
 * async-signal-safe functions - setenv() (malloc + the environ lock)
 * is not one, and a lock another thread held at fork time would hang
 * the child forever while it holds the broker fd. Returns a NULL-
 * terminated vector to free with free_child_env(), or NULL. */
/* The report channel (POST /cool/state) is the running controller's
 * alone. A loopback peer is not enough to say so: anything on this host
 * is a loopback peer. Each spawn gets a secret in its environment, the
 * way it gets the pulse fd, and the route asks for it; a reap ends it.
 * It has a mutex of its own: the route must never wait behind the
 * lifecycle thread. */
#define REPORT_SECRET_HEX 32
static pthread_mutex_t secret_mu = PTHREAD_MUTEX_INITIALIZER;
static char report_secret[REPORT_SECRET_HEX + 1];

static int report_secret_new(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[REPORT_SECRET_HEX / 2];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    int ok = fd >= 0 && read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
    if (fd >= 0)
        close(fd);
    pthread_mutex_lock(&secret_mu);
    for (size_t i = 0; ok && i < sizeof(raw); i++) {
        report_secret[2 * i] = hex[raw[i] >> 4];
        report_secret[2 * i + 1] = hex[raw[i] & 0xf];
    }
    report_secret[ok ? REPORT_SECRET_HEX : 0] = '\0';
    pthread_mutex_unlock(&secret_mu);
    return ok ? 0 : -1;
}

static void report_secret_end(void)
{
    pthread_mutex_lock(&secret_mu);
    memset(report_secret, 0, sizeof(report_secret));
    pthread_mutex_unlock(&secret_mu);
}

int super_report_secret_ok(const char *presented)
{
    unsigned diff = 0;
    size_t n = presented ? strlen(presented) : 0;
    pthread_mutex_lock(&secret_mu);
    int live = report_secret[0] != '\0';
    for (size_t i = 0; i < REPORT_SECRET_HEX; i++)
        diff |= (unsigned char)report_secret[i] ^ (unsigned char)(i < n ? presented[i] : 0);
    pthread_mutex_unlock(&secret_mu);
    return live && n == REPORT_SECRET_HEX && diff == 0;
}

/* What a controller inherits of the daemon's environment: the search path,
 * the locale and the time zone, and the daemon's port, which its reports
 * go to. Nothing else passes on a machine: an override, a bench knob or a
 * proxy setting in the environment of whoever started the daemon reaches
 * no controller. A host-test run (testhook.h) passes every variable, so a
 * host test's stand-ins get their own. */
static int child_env_passes(const char *kv)
{
    static const char *const keep[] = { "PATH", "LANG", "LC_ALL", "TZ", "FORGECTRL_PORT" };
    if (ff_host_test())
        return 1;
    for (size_t i = 0; i < sizeof(keep) / sizeof(*keep); i++) {
        size_t n = strlen(keep[i]);
        if (!strncmp(kv, keep[i], n) && kv[n] == '=')
            return 1;
    }
    return 0;
}

static char **build_child_env(ctl_t ctl, int pulse_fd)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = calloc(n + 4, sizeof(*env));
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        /* Our own entries replace any inherited value of the same key. */
        if (!strncmp(environ[i], "GF_PULSE_FD=", 12) ||
            !strncmp(environ[i], "GF_REPORT_SECRET=", 17) ||
            !strncmp(environ[i], "GFSINK=", 7) ||
            !child_env_passes(environ[i]))
            continue;
        env[k] = strdup(environ[i]);
        if (!env[k])
            goto fail;
        k++;
    }
    if (pulse_fd >= 0) {
        char fdv[32];
        snprintf(fdv, sizeof(fdv), "GF_PULSE_FD=%d", pulse_fd);
        if (!(env[k] = strdup(fdv)))
            goto fail;
        k++;
    }
    const char *dev = pulse_dev();
    if (ctl == Ctl_Grbl && dev) {
        char sink[160];
        snprintf(sink, sizeof(sink), "GFSINK=%s", dev);
        if (!(env[k] = strdup(sink)))
            goto fail;
        k++;
    }
    {
        char sv[24 + REPORT_SECRET_HEX];
        pthread_mutex_lock(&secret_mu);
        snprintf(sv, sizeof(sv), "GF_REPORT_SECRET=%s", report_secret);
        pthread_mutex_unlock(&secret_mu);
        if (!(env[k] = strdup(sv)))
            goto fail;
        k++;
    }
    env[k] = NULL;
    return env;
fail:
    for (size_t i = 0; i < k; i++)
        free(env[i]);
    free(env);
    return NULL;
}

static void free_child_env(char **env)
{
    if (!env)
        return;
    for (size_t i = 0; env[i]; i++)
        free(env[i]);
    free(env);
}

/* The descriptor numbers a child can hold, read before its fork: the
 * fallback below closes up to here. */
static int fd_ceiling(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur > 65536)
        return 65536;
    return (int)rl.rlim_cur;
}

/* Every descriptor past the three standard ones closed but keep (-1: none),
 * in a child between fork() and exec: close_range, and where a kernel or a
 * sandbox refuses it, close() one by one up to ceiling (fd_ceiling, read
 * before the fork). Async-signal-safe. */
static void close_past_stdio(int keep, int ceiling)
{
    int ok;
    if (keep < 3)
        ok = close_range(3, ~0U, 0) == 0;
    else
        ok = (keep == 3 || close_range(3, (unsigned)keep - 1, 0) == 0) &&
             close_range((unsigned)keep + 1, ~0U, 0) == 0;
    if (!ok)
        for (int fd = 3; fd < ceiling; fd++)
            if (fd != keep)
                close(fd);
}

/* A controller's stray stdout/stderr - interpreter tracebacks, library
 * messages, anything not sent through its own syslog emitter - flows
 * through a `logger` relay into syslog under the controller's program
 * name, so it lands in that controller's log directory. The relay is
 * double-forked (init reaps it) and lives as long as the pipe has a
 * writer, so it outlives this daemon whenever the controller does.
 * Returns the pipe's write end, or -1 (the controller then runs with
 * its output discarded rather than not at all). The daemon's stop signals
 * are held across each fork and default in the child (stopsig_fork). */
static int spawn_output_relay(const char *tag)
{
    int p[2];
    int oom = !ff_host_test();
    int ceiling = fd_ceiling();
    if (pipe2(p, O_CLOEXEC) != 0)
        return -1;
    pid_t pid = stopsig_fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        pid_t g = stopsig_fork();
        if (g != 0)
            _exit(g < 0 ? 127 : 0);
        dup2(p[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        close_past_stdio(-1, ceiling);
        /* Reclaimed no sooner than the controller it serves: a dead
         * relay turns the controller's stray writes into EPIPE. */
        int ofd = oom ? open("/proc/self/oom_score_adj", O_WRONLY) : -1;
        if (ofd >= 0) {
            (void)!write(ofd, "-500", 4);
            close(ofd);
        }
        execl("/usr/bin/logger", "logger", "-t", tag, "-p",
              "daemon.warning", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return p[1];
}

/* A controller dies with this daemon. The kernel sends it SIGKILL when the
 * thread that forked it ends, and controllers are forked by the lifecycle
 * thread alone, which ends with the daemon, so no controller outlives the
 * supervisor that safes its deaths. A daemon's stop stops the controller
 * first, busy or idle (super_shutdown), and the thread ends after it; at a
 * crash, or a stop killed before it got there, the signal finds the
 * controller whatever it is doing: its exit closes its share of the pulse
 * device, and with the daemon's share gone too that is the device's last
 * close, the kernel's dead man's switch (a run halted, the heat sources and
 * the head made safe, the latch locked).
 * The exception is a GRBL controller's homing runner, which the controller
 * forks for a $H in a process group of its own: it inherits the pulse
 * device and has no parent-death signal. A daemon's stop ends it with its
 * controller (the safing pair stops its move, and the controller's own stop
 * ends its homing session: SIGTERM to the runner's group, SIGKILL 5 s
 * later, before the controller exits; a runner a killed controller left is
 * ended by the supervisor's stop itself, runner_reap). After a crash, or a
 * stop killed
 * before it got there, it homes (motion, the laser latched) to its own end
 * or its deadline, the device stays open until it exits, and the next
 * daemon stands by until it is gone.
 * A daemon gone before the signal is armed leaves the child another
 * parent: getppid() then names someone else, and it must not run.
 * Async-signal-safe (two system calls), for the child between fork() and
 * exec. Returns 0, or -1 when the controller must not run. */
static int ctl_dies_with_daemon(pid_t daemon)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
        return -1;
    return getppid() == daemon ? 0 : -1;
}

/* Called with mu held. */
static int lamp_pending;            /* the idle lamp is owed after a spawn (under mu) */

/* The settings store path, with the data directory present. */
static void grbl_nvs_path(char *buf, size_t len)
{
    const char *dir = ff_data_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        fflog(LOG_WARNING, "super: mkdir %s: %s", dir, strerror(errno));
    snprintf(buf, len, "%s/%s", dir, GRBL_NVS_FILE);
}

/* The head made safe (head/safe): the measuring laser and the UV LED off,
 * the lens driver disabled at low current; the head keeps its registers
 * across a reset of the SoC, so an emitter left on stays on until written.
 * A head that did not probe has no attribute: that is said once, below the
 * safing writes' severity, and there is nothing to make safe. A write that
 * fails is an error, tried once: the kernel's own write already reads each
 * register back and tries again, and a reap holds mu. Returns 0 when it
 * landed. */
static int head_safe_write(void)
{
    static int missing_said;
    char path[96];
    snprintf(path, sizeof(path), "%shead/safe", sysfs_root());
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        if (!missing_said) {
            missing_said = 1;
            fflog(LOG_NOTICE, "super: no head/safe (the head did not probe): no head to make safe");
        }
        return -1;
    }
    int err = 0;
    if (fd < 0 || write(fd, "1", 1) < 0)
        err = errno;
    if (fd >= 0)
        close(fd);
    if (err) {
        fflog(LOG_ERR, "super: making the head safe failed: %s", strerror(err));
        return -1;
    }
    return 0;
}

/* The head made safe once by this daemon before any controller of its own
 * (under mu): whatever the last controller left on it was reaped by nobody
 * if it died with the last daemon. super_init writes it before the motion
 * probe and the lens sweep; a daemon that found an unmanaged controller
 * at its start leaves that controller's head alone, and what ends the
 * standby writes it, before the probe and the sweep too: the takeover that
 * ends the unmanaged controller (takeover_unmanaged), from the lifecycle's
 * own retake once the machine is idle (the usual end) or from a mode
 * switch, or the mode switch once that controller has ended on its own
 * (switch_takeover). The spawn writes it if none did. */

/* The cloud client's no-hunt marker (gfcloud.py reads it at its start):
 * the test hook GF_NOHUNT_MARKER in a host-test run (testhook.h). */
static const char *nohunt_marker(void)
{
    const char *v = ff_test_hook("GF_NOHUNT_MARKER");
    return v ? v : "/run/gfcloud-nohunt";
}

/* The kind of every controller spawned, recorded in the run directory
 * before the fork. A cloud client whose last controller was not a cloud
 * client (no record included: the first spawn since the boot) hunts the
 * head and the lens at its start: the no-hunt marker goes first. So a
 * switch to cloud mode from GRBL always hunts, whichever way it comes (a
 * mode switch, the cloud wizard's, a daemon start with cloud mode saved, a
 * standby's takeover). Called with mu held. */
static void controller_record(ctl_t ctl)
{
    char path[256], prev[16] = "";
    snprintf(path, sizeof(path), "%s/last-controller", ff_run_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(prev, sizeof(prev), f))
            prev[0] = '\0';
        fclose(f);
    }
    prev[strcspn(prev, "\n")] = '\0';
    if (ctl == Ctl_Cloud && strcmp(prev, "cloud") != 0 && unlink(nohunt_marker()) == 0)
        fflog(LOG_NOTICE, "super: the cloud client follows %s: it hunts at its start (the no-hunt "
                          "marker removed)", prev[0] ? prev : "no recorded controller");
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (mkdir(ff_run_dir(), 0755) != 0 && errno != EEXIST)
        fflog(LOG_WARNING, "super: mkdir %s: %s", ff_run_dir(), strerror(errno));
    f = fopen(tmp, "w");
    int ok = f != NULL;
    if (f) {
        ok = fprintf(f, "%s\n", ctl_name(ctl)) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    if (!ok || rename(tmp, path) != 0) {
        fflog(LOG_WARNING, "super: the controller record could not be written: %s", strerror(errno));
        unlink(tmp);
    }
}

static void spawn_locked(ctl_t ctl)
{
    broker_open_locked();
    /* No broker, no controller (broker_open_locked): one started without
     * the fd would open the device itself. */
    if (broker_fd < 0) {
        respawn_at = wall_s() + BROKER_RETRY_S;
        return;
    }
    const char *bin = ctl_bin(ctl);
    if (!bin) {
        fflog(LOG_ERR, "super: the %s controller's binary override is not an absolute path - no "
                       "controller until it is", ctl_name(ctl));
        respawn_at = wall_s() + BROKER_RETRY_S;
        return;
    }

    /* Before the fork: the controller sets its own head state from its
     * start. */
    if (!head_safed) {
        head_safe_write();
        head_safed = 1;
    }

    controller_record(ctl);

    const char *data_dir = ff_data_dir();
    char nvs[256] = "";
    if (ctl == Ctl_Grbl)
        grbl_nvs_path(nvs, sizeof nvs);

    /* No entropy, no secret, no controller: one whose every report would
     * be refused is a machine that cannot fire and does not say why. */
    if (report_secret_new() != 0) {
        fflog(LOG_ERR, "super: cannot read /dev/urandom for the controller's report secret");
        respawn_at = wall_s() + backoff_s;
        return;
    }
    char **env = build_child_env(ctl, broker_fd);
    if (!env) {
        fflog(LOG_ERR, "super: cannot build the controller "
                       "environment: %s", strerror(errno));
        respawn_at = wall_s() + backoff_s;
        return;
    }

    int oom = !ff_host_test();
    int ceiling = fd_ceiling();
    int lfd = spawn_output_relay(ctl == Ctl_Grbl ? "grblhal" : "gfcloud");
    pid_t daemon = getpid();
    /* A stop's SIGTERM can reach the child before its exec (the lever's or
     * the daemon's stop, sent by another thread the moment this one drops
     * mu). The daemon's stop signals are held across the fork, and the
     * child's first act gives them their default action (stopsig_fork):
     * the signal ends it there, a death its stop already expects
     * (stop_pid), never taken by the daemon's own handler in the child
     * and lost. */
    pid_t pid = stopsig_fork();
    if (pid < 0) {
        fflog(LOG_ERR, "super: fork failed: %s",
              strerror(errno));
        free_child_env(env);
        if (lfd >= 0)
            close(lfd);
        respawn_at = wall_s() + backoff_s;
        return;
    }
    if (pid == 0) {
        /* Child: async-signal-safe calls only from here to exec. The
         * stop signals are at their default action already (above). Own
         * process group so stop() can signal helpers too. Dies with the
         * daemon (ctl_dies_with_daemon). */
        setpgid(0, 0);
        if (ctl_dies_with_daemon(daemon) != 0)
            _exit(125);
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
        }
        if (broker_fd >= 0) {
            /* The broker fd is opened O_CLOEXEC; a controller is the
             * one child that must inherit it across exec. */
            fcntl(broker_fd, F_SETFD, 0);
        }
        /* The broker fd is the only descriptor a controller inherits.
         * Everything else the daemon holds - listening and client
         * sockets, capture and encoder nodes - would otherwise stay
         * pinned for the whole life of the child. Every number, whatever
         * the descriptor limit. */
        close_past_stdio(broker_fd, ceiling);
        /* A controller must be a less-preferred OOM victim than
         * ordinary processes, but a MORE preferred one than the daemon
         * (which is its dead-man and respawns it). */
        int ofd = oom ? open("/proc/self/oom_score_adj", O_WRONLY) : -1;
        if (ofd >= 0) {
            (void)!write(ofd, "-500", 4);
            close(ofd);
        }
        if (ctl == Ctl_Grbl) {
            if (chdir(data_dir) != 0)
                _exit(126);
            /* In the wizard's posture the Grbl port binds to loopback
             * only: the wizard's own streamer reaches it, no sender
             * on the network does. */
            if (local_posture)
                execle(bin, bin, "-p", "23", "-b", "::1",
                       "-e", nvs, (char *)NULL, env);
            else
                execle(bin, bin, "-p", "23",
                       "-e", nvs, (char *)NULL, env);
        } else {
            execle(bin, bin, (char *)NULL, env);
        }
        _exit(127);
    }
    free_child_env(env);
    if (lfd >= 0)
        close(lfd);         /* the controller holds the only writer now */
    child_pid = pid;
    child_ctl = ctl;
    spawned_at = wall_s();
    generation++;
    fflog(LOG_NOTICE, "super: started %s controller (pid %d)",
          ctl_name(ctl), (int)pid);
    /* The idle lid lamp is asserted around every spawn: a cloud client
     * drives its own level while it runs and leaves it behind. Applied
     * by the loop once mu is dropped: the lamp write takes the camera
     * control lock, which a snapshot or a pipeline setup can hold for
     * seconds, and nothing that safes the machine may wait behind it. */
    lamp_pending = 1;
}

/* Reap and, if the death was unexpected, safe the machine and arm the
 * respawn backoff. Called with mu held. */
static void reap_locked(int status)
{
    ctl_t died = child_ctl;
    double up = wall_s() - spawned_at;
    /* A death the supervisor asked for (stop_locked: the lever, a takeover,
     * a mode switch, the gate closing, a posture change, the fail tier, the
     * shutdown) is expected, whatever stands once it lands. */
    int deliberate = stop_pid != 0 && stop_pid == child_pid;
    stop_pid = 0;
    report_secret_end();            /* nobody reports for a controller that is gone */
    child_pid = 0;
    child_ctl = Ctl_None;
    generation++;
    pthread_cond_broadcast(&cv);

    int expected = deliberate || suspended || want != died || restart_pending;
    fflog(expected ? LOG_NOTICE : LOG_WARNING,
          "super: %s controller exited (status 0x%x, up %.0f s)%s",
          ctl_name(died), (unsigned)status, up,
          expected ? "" : " - unexpected");

    /* Safe posture on EVERY transition out of a running child, expected
     * or not. Under the device broker a child's exit is not a final
     * close of the pulse device, so the kernel's close-relocks backstop
     * never fires for managed controllers - these two writes are the
     * real safing mechanism, and both are harmless when the machine is
     * already idle and latched. */
    safing_pair();
    /* Then the live-feed declaration, behind the pair and never ahead of
     * it: the controller is gone and declares nothing again. One that died
     * mid-stream leaves cnc/streaming at 1: the ring it left draining would
     * end in an underrun instead of idle, and the next holder would start
     * on a live feed it never declared. */
    wr_attr("cnc/streaming", "0");
    /* Then the head, which no close of the device reaches here either: the
     * measuring laser and the UV LED off, the lens driver disabled at low
     * current (head/safe). An emitter the controller left on would
     * otherwise stay on, into the next boot, since the head keeps its
     * registers across a reset of the SoC. Before any respawn: the next
     * controller sets its own. */
    head_safe_write();
    /* The homing anchor belongs to the controller that wrote it: it
     * must not survive into another mode (cloud re-zeros the counters
     * it anchors) or into a respawn. A fresh GRBL controller starts
     * unreferenced and re-homes. */
    unlink(HOMED_ANCHOR);
    /* So does the lens reference: a controller moves the lens (a sender's
     * Z, a camera home's park, a cloud session's own homing), so the edge
     * the marker names is the lens's position no longer. The next GRBL
     * controller gets a fresh sweep before it starts. */
    lenshome_clear();
    /* A GRBL controller may have left its homing runner behind, alive
     * on the inherited fd: the loop finds and ends it before anything
     * else may write the ring. */
    runner_check = died == Ctl_Grbl;
    /* The kernel may still be playing what the controller queued: the
     * respawn waits for it (bounded) so the new controller's init never
     * overlaps a ramp, and the engine forgets the dead reporter the way
     * it forgets a stopped one, so the hang dead-man does not count the
     * silence of a controller that is already gone. */
    cool_controller_stopped();
    respawn_wait_idle = 1;
    died_at = wall_s();

    if (expected)
        return;

    if (up >= HEALTHY_UPTIME_S)
        backoff_s = RESPAWN_MIN_S;
    respawn_at = wall_s() + backoff_s;
    fflog(LOG_WARNING, "super: respawn in %u s", backoff_s);
    backoff_s = backoff_s * 2 < RESPAWN_MAX_S ? backoff_s * 2 : RESPAWN_MAX_S;
}

/* The stop's SIGTERM from the thread that asks for the stop (the lever,
 * the daemon's stop), right after its safing pair: the controller's own
 * handler ends its run in order (the kernel's ramp, the latch relocked) and
 * leaves, whatever the lifecycle thread is doing; one still before its exec
 * (spawn_locked) is ended by the signal there. The lifecycle thread's
 * stop_locked follows: the pair and the signal again (a repeated SIGTERM is
 * a no-op in both controllers' handlers), SIGKILL past its grace, the reap.
 * The pid is marked first, so its death is expected whichever thread reaps
 * it. Called with mu held. */
static void term_child_locked(void)
{
    if (child_pid <= 0)
        return;
    stop_pid = child_pid;
    kill(-child_pid, SIGTERM);
    kill(child_pid, SIGTERM);
    cool_controller_stopped();
}

/* Stop the child: SIGTERM its group, escalate to SIGKILL. Called with
 * mu held; drops and retakes the lock while waiting (the thread's
 * waitpid runs reap_locked). */
static void stop_locked(void)
{
    if (child_pid <= 0)
        return;
    pid_t pid = child_pid;
    stop_pid = pid;
    fflog(LOG_INFO, "super: stopping %s controller (pid %d)",
          ctl_name(child_ctl), (int)pid);
    /* Safe the machine BEFORE the signal, not only after the reap: this
     * path is also the emergency lever (POST /controller/stop is not
     * idle-gated), and a controller that is mid-job may take its whole
     * SIGTERM grace to leave. cnc/stop is a controlled deceleration and
     * the latch relock severs FIRE - both instantaneous at the kernel,
     * both harmless no-ops when the machine is already idle and latched. */
    safing_pair();
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    /* Once more after the signal: a write that failed its retries the
     * first time gets a second chance while the controller leaves. */
    safing_pair();
    /* The cooling engine's hang dead-man watches the seconds since the
     * controller's last report; this stop is deliberate, so the report
     * is forgotten and the liveness probe that may follow plays with
     * nobody counting. */
    cool_controller_stopped();

    double deadline = wall_s() + STOP_TERM_WAIT_S;
    while (child_pid == pid) {
        pthread_mutex_unlock(&mu);
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        pthread_mutex_lock(&mu);
        if (r == pid) {
            reap_locked(status);
            break;
        }
        /* Reaped meanwhile by the supervisor thread (shutdown runs this
         * from the caller's thread while that loop may still be
         * draining): the pid is free for reuse and must not be
         * signaled again. */
        if (child_pid != pid || (r < 0 && errno == ECHILD))
            break;
        if (wall_s() > deadline) {
            fflog(LOG_WARNING, "super: escalating to SIGKILL");
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            /* A SIGKILLed child can run no cleanup of its own - safe
             * the machine now rather than waiting for the reap. Behind
             * the pair, the live-feed declaration goes as it does at the
             * reap: a killed child declares nothing again, and the reap
             * writes it once more. */
            safing_pair();
            wr_attr("cnc/streaming", "0");
            deadline = wall_s() + STOP_TERM_WAIT_S;
        }
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
}

/* The lens back on its hall edge before a GRBL controller that would
 * otherwise start with no marker: every controller exit drops it, and a
 * hold whose probe was skipped never took one. Called with mu held, no
 * controller alive, supervision not suspended, behind the enclosure
 * check; probing is set before the lock is dropped, so a stop lever, a
 * takeover or the daemon's shutdown from here on reaches the sweep
 * (sweep_stop, between two steps), or found supervision suspended first
 * and no sweep began; the lever and the takeover wait for its end. A lid or the interlock opening, or switches that stop
 * answering, end it too. A hunt that finds no edge leaves the marker
 * absent, and the controller then holds Z unreferenced: no motion fault,
 * the lens is never declared where it may not be. Such a hunt is not
 * repeated in this hold (a sensor stuck clear would drive the carriage
 * onto its stop at every start) until a fresh hold or a hunt that finds
 * the edge. A standing motor release does not skip it: the release is X
 * and Y's, the lens motor stays at its hold current, and the sweep writes
 * the lens motor's attributes alone, behind the same enclosure check.
 * 1 when the spawn goes ahead, 0 when it waits: the sweep was ended,
 * supervision suspended, or the enclosure is open. */
static int lens_sweep_locked(void)
{
    if (sweep_failed) {
        fflog(LOG_NOTICE, "super: lens reference before the controller: SKIPPED - a sweep found "
                          "no edge in this hold; Z stays unreferenced");
        return 1;
    }
    probing = 1;
    probe_abort = 0;
    pthread_mutex_unlock(&mu);
    char lens[80], why[96];
    int lrc = -3;
    if (machine_is_idle())
        lrc = lenshome_run(lens, sizeof(lens), sweep_stop);
    else
        snprintf(lens, sizeof(lens), "the kernel is not idle");
    int enc = liveness_enclosure(why, sizeof(why));
    pthread_mutex_lock(&mu);
    probing = 0;
    pthread_cond_broadcast(&cv);
    if (lrc == 0)
        sweep_failed = 1;
    fflog(lrc == 1 ? LOG_INFO : lrc == LENSHOME_STOPPED ? LOG_NOTICE : LOG_WARNING,
          "super: lens reference before the controller: %s - %s%s",
          lrc == 1 ? "ON EDGE" : lrc == LENSHOME_STOPPED ? "ENDED" : lrc == -1 ? "SKIPPED" : "NONE", lens,
          lrc == 1 || lrc == LENSHOME_STOPPED || lrc == -1 ? "" : "; Z stays unreferenced");
    if (enc != 0) {
        wait_enter_locked(why);
        return 0;
    }
    if (lrc == LENSHOME_STOPPED || suspended)
        return 0;
    return 1;
}

/* ------------------------------------------------------ thread body */

/* One pass of the lifecycle. Called with mu held; drops it around
 * everything slow. */
static void pass_locked(void)
{
    {
        /* Reap. A SIGCHLD woke the loop for this, so the safing in
         * reap_locked lands within milliseconds of the exit. */
        if (child_pid > 0) {
            pid_t pid = child_pid;
            pthread_mutex_unlock(&mu);
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            pthread_mutex_lock(&mu);
            if (r == pid)
                reap_locked(status);
        }
        /* The engine's fail tier: stop the controller the deliberate
         * way (the safing pair before the signal) and start it again
         * at once - the job cannot go on, and the sender gets a clean
         * disconnect instead of a dark job. */
        if (restart_pending) {
            if (child_pid > 0) {
                fflog(LOG_WARNING, "super: stopping the %s controller on the "
                                   "engine's fail tier", ctl_name(child_ctl));
                stop_locked();
            }
            restart_pending = 0;
            respawn_at = 0.0;
        }
        /* A GRBL controller's homing runner must not outlive it. */
        if (runner_check) {
            runner_check = 0;
            pthread_mutex_unlock(&mu);
            runner_reap();
            pthread_mutex_lock(&mu);
        }
        /* Standing by next to an unmanaged controller (found at start: a
         * hand-started one, or a homing runner a crashed daemon's GRBL
         * controller left behind): retake supervision
         * as soon as the kernel plays nothing (idle, disabled, a stepper
         * fault, an underrun) - a busy one is left to finish its job
         * first. The probe that follows acknowledges an underrun and
         * clears a fault whose lines have cleared, or names one that
         * stays. */
        if (standby_takeover && child_pid == 0 && want != Ctl_None && !(suspended & SUSP_SHUTDOWN)) {
            pthread_mutex_unlock(&mu);
            int idle = !kernel_playing() && !diag_running();
            if (idle)
                takeover_unmanaged();
            pthread_mutex_lock(&mu);
            if (idle) {
                standby_takeover = 0;
                suspended &= ~SUSP_STANDBY;
            }
        }

        /* The setup gate (the advisories, the account, the required
         * steps, and the intake fan count): no controller for a sender
         * while it is closed. A
         * wizard's own controller (local posture, loopback only) passes.
         * A gate that closes on a running controller (an advisory changed
         * by an update) stops it once the machine is idle, never
         * mid-job. */
        {
            char why[256];
            pthread_mutex_unlock(&mu);
            int open = controller_gate_open(why, sizeof(why));
            pthread_mutex_lock(&mu);
            int now_gated = !open && !local_posture;
            if (now_gated != gated || (now_gated && strcmp(why, gate_why))) {
                fflog(now_gated ? LOG_WARNING : LOG_NOTICE,
                      "super: controller gate %s%s%s",
                      now_gated ? "closed" : "open",
                      now_gated ? ": " : "", now_gated ? why : "");
            }
            gated = now_gated;
            snprintf(gate_why, sizeof(gate_why), "%s", why);
        }
        if (child_pid > 0 && gated) {
            pthread_mutex_unlock(&mu);
            int idle = machine_is_idle();
            pthread_mutex_lock(&mu);
            if (idle && child_pid > 0)
                stop_locked();
        }

        /* Converge on the wanted state. Every spawn passes the
         * enclosure check first; the first spawn after taking the
         * broker passes the motion-liveness gate as well (unlocked -
         * the probe and its recovery ladder take a while). Nothing of
         * it runs before the daemon's listener is up. */
        if (child_pid > 0 && (suspended || child_ctl != want)) {
            stop_locked();
        } else if (child_pid == 0 && !suspended && !motion_fault && !gated && !await_listener
                   && want != Ctl_None && wall_s() >= respawn_at) {
            int go = 1;
            if (respawn_wait_idle) {
                /* The kernel finishes what the dead controller queued
                 * before another controller may touch it. A kernel
                 * that has not gone idle in the bound is halted: an
                 * endless ramp is not a state to hand over. A stepper
                 * fault plays nothing and a halt cannot end it: no
                 * controller starts into it. It is a motion fault in its
                 * own words, and the probe a retry runs gives it its one
                 * enable and verifies the gantry after it. An underrun
                 * plays nothing and nothing in it is waited for: it is
                 * acknowledged at once with the stop, as the probe does,
                 * and the respawn goes ahead once the stop is taken. A stop
                 * the kernel refuses (a fault that landed after the read)
                 * leaves the wait standing: the next pass reads the state
                 * again, and a fault takes its branch. */
                pthread_mutex_unlock(&mu);
                int idle = machine_is_idle();
                char st[24] = "", faults[16] = "";
                if (!idle && rd_attr("cnc/state", st, sizeof(st)) == 0 && !strcmp(st, "fault"))
                    rd_attr("cnc/faults", faults, sizeof(faults));
                pthread_mutex_lock(&mu);
                if (suspended)
                    go = 0;     /* a stop landed while the lock was down: the next pass decides */
                else if (idle)
                    respawn_wait_idle = 0;
                else if (!strcmp(st, "fault")) {
                    respawn_wait_idle = 0;
                    go = 0;
                    probed = 0;
                    probe_skipped = 0;
                    motion_fault = 1;
                    snprintf(probe_detail, sizeof(probe_detail), "stepper fault (faults %s): Retry, "
                             "or the Motion check, clears it", faults[0] ? faults : "unreadable");
                    generation++;
                    fflog(LOG_CRIT, "super: MOTION FAULT - %s; no controller until then", probe_detail);
                }
                else if (!strcmp(st, "underrun")) {
                    if (wr_attr("cnc/stop", "1") == 0) {
                        fflog(LOG_NOTICE, "super: the kernel reads underrun after the controller left - "
                                          "acknowledged before the respawn");
                        respawn_wait_idle = 0;
                    }
                    else
                        go = 0;
                }
                else if (wall_s() - died_at < RESPAWN_IDLE_WAIT_S)
                    go = 0;
                else {
                    fflog(LOG_CRIT, "super: the kernel's state is still %s %d s "
                                    "after the controller left - halting it "
                                    "before the respawn", st[0] ? st : "unreadable",
                          RESPAWN_IDLE_WAIT_S);
                    wr_attr("cnc/halt", "1");
                    respawn_wait_idle = 0;
                }
            }
            if (go) {
                broker_open_locked();
                if (broker_fd < 0) {
                    /* No broker: no controller. Try again soon. */
                    respawn_at = wall_s() + BROKER_RETRY_S;
                } else {
                    /* Nothing spawns with a lid or the interlock open,
                     * or with the switches unreadable: the gate waits
                     * for the enclosure and says so, and looks again
                     * every pass. The probe (once per broker hold)
                     * moves the gantry, so it runs behind the same
                     * check; a busy kernel or a stop makes it wait. */
                    char why[96];
                    pthread_mutex_unlock(&mu);
                    int enc = liveness_enclosure(why, sizeof(why));
                    pthread_mutex_lock(&mu);
                    if (suspended || motion_fault || gated || child_pid > 0) {
                        /* The lock was down for the read: a stop lever, a
                         * check's takeover, a fault or the gate landed in
                         * it. Nothing moves and nothing starts on what
                         * the pass saw before; the next pass decides.
                         * From here to the motion below the lock is held,
                         * and the probe and the sweep set probing before
                         * they drop it, so the lever and the takeover
                         * reach them. */
                    } else if (enc != 0) {
                        wait_enter_locked(why);
                    } else if (!probed) {
                        wait_leave_locked("runs: the enclosure is closed");
                        int fd = broker_fd;
                        probing = 1;
                        probe_abort = 0;
                        pthread_mutex_unlock(&mu);
                        int rc = probe_sequence(fd);
                        pthread_mutex_lock(&mu);
                        probing = 0;
                        pthread_cond_broadcast(&cv);
                        if (rc == 3)
                            respawn_at = wall_s() + 1.0;    /* look again in a moment */
                        else if (rc != 4) {
                            probed = rc != 0;
                            probe_skipped = rc == 2;
                            motion_fault = rc == 0;
                        }
                    } else {
                        wait_leave_locked("no longer waits: the enclosure is closed");
                        /* A marker standing is a hunt that found the edge
                         * since the last controller (a check's, a burn's):
                         * a failed sweep earlier in the hold is history. */
                        int marker = lenshome_present();
                        if (marker)
                            sweep_failed = 0;
                        if (want != Ctl_Grbl || marker || lens_sweep_locked())
                            spawn_locked(want);
                    }
                }
            }
        } else if (enclosure_wait) {
            wait_leave_locked(suspended ? "no longer waits: supervision suspended"
                              : gated ? "no longer waits: controllers gated"
                              : "no longer waits");
        }
    }
}

static void *super_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    while (th_run) {
        pass_locked();
        int lamp = lamp_pending;
        lamp_pending = 0;
        pthread_mutex_unlock(&mu);
        if (lamp)
            cam_lamp_apply_idle();
        loop_wait(LOOP_TICK_MS);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* Wait until the lifecycle state satisfies pred (generation-driven).
 * Called with mu held; returns 0, or -1 on timeout. */
static int wait_for(int (*pred)(void), double timeout_s)
{
    double deadline = wall_s() + timeout_s;
    while (!pred()) {
        if (wall_s() > deadline)
            return -1;
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
    return 0;
}

static int pred_child_gone(void)  { return child_pid == 0 && !probing; }

/* --------------------------------------------------------------- api */

static ctl_t configured_mode(void)
{
    char v[16];
    /* Cloud mode exists only once the owner turned it on: with
     * cloud_enabled unset, a stale controller_mode=cloud is grbl. */
    if (settings_get("controller_mode", v, sizeof(v)) == 0
        && strcmp(v, "cloud") == 0 && settings_get_bool("cloud_enabled", 0))
        return Ctl_Cloud;
    return Ctl_Grbl;
}

/* A mode switch over a standby or a suspension: an unmanaged controller
 * still running is taken over (takeover_unmanaged, which makes the head
 * safe), whether or not a POST /controller/start has lifted the
 * suspension since; a standby's unmanaged controller that has ended by
 * itself left its head to nobody, so it is made safe here. Either way
 * before the motion probe and the lens sweep the switch's controller comes
 * up behind. Called with mu NOT held; the process search runs before the
 * lock is taken, so no taker of the lock waits behind it. */
static int unmanaged_controller_running(void);
static void switch_takeover(void)
{
    int running = unmanaged_controller_running();
    pthread_mutex_lock(&mu);
    int standby = standby_takeover;
    int takeover = (suspended || standby) && running;
    pthread_mutex_unlock(&mu);
    if (takeover) {
        takeover_unmanaged();
    } else if (standby && !running) {
        pthread_mutex_lock(&mu);
        head_safe_write();
        head_safed = 1;
        pthread_mutex_unlock(&mu);
    }
}

/* A controller process that is not the supervisor's own child: the
 * managed controller answers the same search, and is left out of it (it
 * ends through stop_locked, never through the takeover's signals). The
 * cloud client's pattern is anchored at the command line's start, so it
 * never matches the sh -c wrapper that runs the search. */
static int unmanaged_controller_running(void)
{
    pthread_mutex_lock(&mu);
    int own = (int)child_pid;
    pthread_mutex_unlock(&mu);
    char grbl[96], cloud[160];
    snprintf(grbl, sizeof(grbl), "pgrep -x grblHAL_glowfor | grep -qvx '%d'", own);
    snprintf(cloud, sizeof(cloud), "pgrep -f " CLOUD_CLIENT_RE " | grep -qvx '%d'", own);
    return system(grbl) == 0 || system(cloud) == 0 ||
           system("pgrep -x gfhome.py >/dev/null 2>&1") == 0;
}

/* Controller death is a signal, not a poll: SIGCHLD wakes the
 * lifecycle thread through an eventfd (the handler may only write). */
void super_sigchld_init(void)
{
    if (chld_efd >= 0)
        return;
    chld_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (chld_efd < 0) {
        fflog(LOG_ERR, "super: eventfd: %s - controller deaths are polled",
              strerror(errno));
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        fflog(LOG_ERR, "super: sigaction(SIGCHLD): %s", strerror(errno));
}

/* The supervisor's start, before the lifecycle thread runs, given whether
 * an unmanaged controller runs (looked for before the lock is taken).
 * Called with mu held. */
static void init_locked(int unmanaged)
{
    want = configured_mode();
    if (unmanaged) {
        suspended |= SUSP_STANDBY;
        standby_takeover = 1;
        fflog(LOG_WARNING, "super: unmanaged controller already "
                           "running - standing by until the machine is idle");
    } else {
        /* Before the lifecycle thread's probe and sweep (head_safed). */
        head_safe_write();
        head_safed = 1;
    }
}

void super_init(void)
{
    super_sigchld_init();
    int unmanaged = unmanaged_controller_running();
    pthread_mutex_lock(&mu);
    init_locked(unmanaged);
    await_listener = 1;
    th_run = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, NULL, super_main, NULL) != 0) {
        th_run = 0;
        fflog(LOG_ERR, "super: thread failed to start");
        return;
    }
    fflog(LOG_INFO, "super: supervising mode %s%s",
          ctl_name(want), suspended ? " (standby)" : "");
}

/* The daemon's HTTP listener is up (main.c): the motion probe, the lens
 * sweep and the controller may run from here on. */
void super_listening(void)
{
    pthread_mutex_lock(&mu);
    await_listener = 0;
    pthread_mutex_unlock(&mu);
    wake();
}

/* The daemon's stop begins (main.c, before the listeners close). The safing
 * pair lands first, from this thread, before it waits on anything, the
 * supervisor's lock included: a running job decelerates and FIRE is severed
 * at the kernel at once, whatever the lifecycle thread is doing. Then
 * nothing starts from here on, a live controller is sent its SIGTERM from
 * this thread (term_child_locked), and the lifecycle thread is woken to
 * finish the stop the deliberate way. A probe or a lens sweep in flight
 * ends between its steps, as at the stop lever, never cut off mid-step with
 * the lens motor at its drive current. A wait that would hold a connection
 * thread (a mode switch) ends. Called once or more: the pair is a harmless
 * no-op again, and at idle. */
void super_shutdown_begin(void)
{
    safing_pair();
    pthread_mutex_lock(&mu);
    int first = !(suspended & SUSP_SHUTDOWN);
    suspended |= SUSP_SHUTDOWN;
    if (probing) {
        probe_abort = 1;
        wr_attr("cnc/stop", "1");   /* a probe run begun since the first write */
    }
    if (first)
        term_child_locked();
    wait_leave_locked("no longer waits: shutdown");
    pthread_mutex_unlock(&mu);
    if (first)
        wake();
}

static int pred_stopped(void) { return child_pid == 0 && !probing; }

/* The daemon's stop ends (main.c, after the listeners): the controller
 * stopped, busy or idle, the deliberate way (stop_locked: the safing pair,
 * SIGTERM, SIGKILL past its grace), and only then the lifecycle thread and
 * the broker. A controller dies with the thread that forked it
 * (ctl_dies_with_daemon), so that thread runs until the controller is
 * reaped: its end would otherwise SIGKILL a controller still inside its
 * SIGTERM grace, finishing its job. The thread does the stop; one that does
 * not get to it within SHUTDOWN_WAIT_S (blocked elsewhere) is overtaken by
 * this thread's own stop_locked. The broker closes after the thread has
 * ended, so a probe in flight never writes a closed number. A GRBL
 * controller's homing runner ends with its controller (ctl_dies_with_daemon);
 * one that outlived it (the controller killed past its grace while the
 * runner still had its own) is looked for and ended here, once the thread
 * has ended, before the broker closes: the thread leaves at the reap,
 * without the pass that would look. */
void super_shutdown(void)
{
    super_shutdown_begin();
    pthread_mutex_lock(&mu);
    int was = th_run;
    if (wait_for(pred_stopped, SHUTDOWN_WAIT_S) != 0)
        fflog(LOG_ERR, "super: the lifecycle thread did not end the controller and the probe in "
                       "%d s - the shutdown stops what is left itself", SHUTDOWN_WAIT_S);
    if (child_pid > 0)
        stop_locked();
    th_run = 0;
    pthread_mutex_unlock(&mu);
    wake();
    if (was)
        pthread_join(th, NULL);
    pthread_mutex_lock(&mu);
    int runner = runner_check;
    runner_check = 0;
    pthread_mutex_unlock(&mu);
    if (runner)
        runner_reap();
    pthread_mutex_lock(&mu);
    /* Our reference goes last: the device's last close, the kernel locks
     * the latch. */
    if (broker_fd >= 0) {
        close(broker_fd);
        broker_fd = -1;
    }
    pthread_mutex_unlock(&mu);
}

int super_mode_switch(const char *mode, const char *as, char *err, size_t elen)
{
    ctl_t target;
    if (!strcmp(mode, "grbl"))
        target = Ctl_Grbl;
    else if (!strcmp(mode, "cloud"))
        target = Ctl_Cloud;
    else {
        snprintf(err, elen, "mode must be grbl or cloud");
        return -1;
    }
    if (target == Ctl_Cloud && !settings_get_bool("cloud_enabled", 0)) {
        snprintf(err, elen, "cloud mode is not enabled on this machine");
        return -1;
    }
    {
        char why[256];
        int open = controller_gate_open(why, sizeof(why));
        pthread_mutex_lock(&mu);
        int local = local_posture, stopping = (suspended & SUSP_SHUTDOWN) != 0;
        pthread_mutex_unlock(&mu);
        if (stopping) {
            snprintf(err, elen, "forgectrl is stopping");
            return -1;
        }
        if (!open && !local) {
            snprintf(err, elen, "controllers are gated: %s", why);
            return -1;
        }
    }

    /* Whoever has the machine refuses the switch, unless it is the one
     * asking (the cloud wizard switches inside its own hold). */
    if (lease_refusal_for(as, err, elen))
        return -1;
    if (!machine_is_idle()) {
        snprintf(err, elen, "machine is not idle");
        return -1;
    }
    if (target == Ctl_Cloud && motors_released()) {
        snprintf(err, elen, "the X and Y motors are released: energize them first");
        return -1;
    }

    switch_takeover();

    if (settings_set("controller_mode", mode) != 0) {
        snprintf(err, elen, "cannot persist controller_mode");
        return -1;
    }

    pthread_mutex_lock(&mu);
    want = target;
    suspended &= SUSP_SHUTDOWN;     /* the daemon's stop outlives a switch */
    standby_takeover = 0;
    motion_fault = 0;   /* a switch is also the retry lever after a fault */
    /* The thread stops the old controller and starts the new one; wait
     * until the running child IS the target. A pending liveness
     * (re-)probe runs first, so allow for it. A probe that ends in a
     * motion fault, and the daemon's stop, end the wait with their own
     * answers. */
    double sw_deadline = wall_s() + 90.0;
    while (!(child_pid > 0 && child_ctl == target)) {
        if (suspended & SUSP_SHUTDOWN) {
            pthread_mutex_unlock(&mu);
            snprintf(err, elen, "forgectrl is stopping");
            return -1;
        }
        if (motion_fault) {
            snprintf(err, elen, "motion fault: %s", probe_detail);
            pthread_mutex_unlock(&mu);
            return -1;
        }
        if (enclosure_wait) {
            /* The gate waits for the enclosure: the switch itself is
             * done (the mode is stored, the controller starts when the
             * lid closes) and the reply carries the waiting state. */
            fflog(LOG_NOTICE, "super: mode %s selected; %s - the controller "
                              "starts when it is closed", mode, wait_why);
            pthread_mutex_unlock(&mu);
            return 0;
        }
        if (wall_s() > sw_deadline) {
            pthread_mutex_unlock(&mu);
            snprintf(err, elen, "controller did not start");
            return -1;
        }
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
    double t_spawn = spawned_at;
    pthread_mutex_unlock(&mu);

    /* Wait for the controller to report in to the cooling engine - a
     * report NEWER than the spawn, so the previous controller's last
     * report cannot satisfy the wait. The cloud client signs into the
     * web service first, so give it time; a slow first report is a
     * warning, not a failure - but a child that keeps dying (bad
     * binary, immediate crash) is one. */
    double deadline = wall_s() + REPORT_WAIT_S;
    while (wall_s() < deadline) {
        double age = cool_report_age();
        if (age >= 0.0 && age < wall_s() - t_spawn)
            return 0;
        pthread_mutex_lock(&mu);
        int dead = child_pid == 0 || child_ctl != target;
        pthread_mutex_unlock(&mu);
        if (dead) {
            snprintf(err, elen,
                     "%s controller keeps exiting - check its log", mode);
            return -1;
        }
        usleep(500 * 1000);
    }
    fflog(LOG_INFO, "super: %s controller running but no "
                    "job-state report yet", mode);
    return 0;
}

/* Suspend for one reason, wake the lifecycle thread, and wait for the
 * controller to be gone. A probe or a lens sweep in flight is the machine
 * moving with no controller: the stop reaches it too, between its steps.
 * The stop lever lands the safing pair from this thread before it waits,
 * and then sends the controller its SIGTERM (term_child_locked), so the run
 * in progress decelerates, FIRE is severed at the kernel and the
 * controller's own stop follows at once, whatever the lifecycle thread is
 * doing; beside an unmanaged controller (standing by) it answers 1 at once:
 * the pair is in, the hold stands, and the lifecycle's takeover ends that
 * controller once the machine is idle. A wait that runs out leaves the
 * lever's hold standing (the lifecycle ends the controller when it gets to
 * it) and answers -1; a takeover that runs out drops its reason if this
 * call set it, and answers -1. */
static int suspend_for(unsigned reason)
{
    pthread_mutex_lock(&mu);
    int had = (suspended & reason) != 0;
    suspended |= reason;
    if (reason == SUSP_HOLD) {
        safing_pair();
        term_child_locked();
    }
    if (probing) {
        probe_abort = 1;
        wr_attr("cnc/stop", "1");
    }
    int beside = reason == SUSP_HOLD && standby_takeover && child_pid == 0 && !probing;
    pthread_mutex_unlock(&mu);
    wake();
    if (beside)
        return 1;
    pthread_mutex_lock(&mu);
    int ok = wait_for(pred_child_gone, LEVER_WAIT_S) == 0;
    if (!ok && reason == SUSP_HOLD)
        fflog(LOG_WARNING, "super: the controller did not exit in %d s of its stop - the stop stands",
              LEVER_WAIT_S);
    else if (!ok && !had)
        suspended &= ~reason;   /* the takeover did not begin: nobody ends it */
    pthread_mutex_unlock(&mu);
    return ok ? 0 : -1;
}

int super_controller_stop(void)
{
    return suspend_for(SUSP_HOLD);
}

void super_controller_start(void)
{
    pthread_mutex_lock(&mu);
    suspended &= SUSP_SHUTDOWN;     /* the daemon's stop outlives a start */
    motion_fault = 0;
    respawn_at = 0.0;
    backoff_s = RESPAWN_MIN_S;
    pthread_mutex_unlock(&mu);
    wake();
}

int super_takeover_begin(void)
{
    return suspend_for(SUSP_TAKEOVER);
}

unsigned super_holds(void)
{
    pthread_mutex_lock(&mu);
    unsigned h = ((suspended & SUSP_HOLD) ? SUPER_HELD : 0) | (motion_fault ? SUPER_FAULTED : 0);
    pthread_mutex_unlock(&mu);
    return h;
}

void super_takeover_end(void)
{
    pthread_mutex_lock(&mu);
    suspended &= ~SUSP_TAKEOVER;
    respawn_at = 0.0;
    backoff_s = RESPAWN_MIN_S;
    pthread_mutex_unlock(&mu);
    wake();
}

void super_controller_restart(const char *why)
{
    pthread_mutex_lock(&mu);
    int had = child_pid > 0;
    restart_pending = 1;
    pthread_mutex_unlock(&mu);
    fflog(LOG_WARNING, "super: %s - the controller is stopped and started again%s",
          why, had ? "" : " (none running)");
    wake();
}

int super_grbl_running(void)
{
    return super_grbl_pid() > 0;
}

const char *super_ctl_agreed(void)
{
    pthread_mutex_lock(&mu);
    const char *ctl = child_pid <= 0 || child_ctl != want ? NULL
                    : child_ctl == Ctl_Grbl ? "grbl" : child_ctl == Ctl_Cloud ? "cloud" : NULL;
    pthread_mutex_unlock(&mu);
    return ctl;
}

long super_grbl_pid(void)
{
    pthread_mutex_lock(&mu);
    long pid = child_pid > 0 && child_ctl == Ctl_Grbl ? (long)child_pid : 0;
    pthread_mutex_unlock(&mu);
    return pid;
}

int super_status_json(char *buf, size_t len)
{
    /* stepper_fault is what the kernel holds under a motion fault, however
     * the fault was found: POST /mode refuses a kernel in fault, and the
     * panel's Retry goes by the controller start instead. Read before the
     * lock, as every sysfs read here is. */
    char kst[24];
    int kfault = rd_attr("cnc/state", kst, sizeof(kst)) == 0 && strcmp(kst, "fault") == 0;
    pthread_mutex_lock(&mu);
    /* why: what holds the machine (the gate's reason, or what is open
     * while the motion check waits), else the probe's own words behind
     * a faulted or unverified verdict. */
    const char *src = gated ? gate_why
                    : enclosure_wait ? wait_why
                    : (motion_fault || probe_skipped) ? probe_detail : "";
    char why[300];
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < sizeof(why); p++) {
        if (*p == '"' || *p == '\\')
            why[o++] = '\\';
        why[o++] = *p;
    }
    why[o] = '\0';
    snprintf(buf, len,
             "{\"mode\":\"%s\",\"controller\":\"%s\",\"pid\":%d,"
             "\"motion\":\"%s\",\"stepper_fault\":%s,\"gated\":%s,\"local\":%s,\"why\":\"%s\"}",
             ctl_name(want),
             child_pid > 0 ? "running"
                 : gated ? "gated"
                 : motion_fault ? "motion-fault"
                 : enclosure_wait ? "waiting"
                 : suspended ? "standby" : "stopped",
             (int)child_pid,
             motion_fault ? "fault"
                 : probed && !probe_skipped ? "verified" : "unverified",
             motion_fault && kfault ? "true" : "false",
             gated ? "true" : "false", local_posture ? "true" : "false",
             why);
    pthread_mutex_unlock(&mu);
    return 0;
}

int super_probe_motion(char *detail, size_t dlen)
{
    pthread_mutex_lock(&mu);
    if (child_pid > 0) {
        pthread_mutex_unlock(&mu);
        snprintf(detail, dlen, "a controller is running");
        return 2;
    }
    if (probing) {
        pthread_mutex_unlock(&mu);
        snprintf(detail, dlen, "a motion probe is already running");
        return 2;
    }
    /* A stop lever pulled, or the daemon's stop begun, before the probe
     * begins: nothing moves after it answered. From here on it finds
     * probing set and reaches it. */
    if (suspended & (SUSP_HOLD | SUSP_SHUTDOWN)) {
        pthread_mutex_unlock(&mu);
        snprintf(detail, dlen, "motion probe ended by the stop lever");
        return 4;
    }
    probing = 1;
    probe_abort = 0;
    pthread_mutex_unlock(&mu);
    /* A stopped GRBL controller's homing runner may still hold the
     * inherited fd and write the ring: it is gone before the probe
     * writes it, as it is before a respawn. */
    runner_reap();
    pthread_mutex_lock(&mu);
    broker_open_locked();
    int fd = broker_fd;
    if (fd < 0) {
        probing = 0;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        snprintf(detail, dlen, "the pulse device is not held");
        return 2;
    }
    /* The verdict before this probe does not outlive it: a probe that
     * ends before a verdict (the enclosure, a busy kernel, the lever)
     * leaves motion unverified, and the lifecycle probes again from the
     * first rung before any controller starts. */
    probed = 0;
    probe_skipped = 0;
    pthread_mutex_unlock(&mu);
    int rc = probe_sequence(fd);
    pthread_mutex_lock(&mu);
    probing = 0;
    pthread_cond_broadcast(&cv);
    if (rc != 3 && rc != 4) {
        probed = rc != 0;
        probe_skipped = rc == 2;
        motion_fault = rc == 0;
    }
    snprintf(detail, dlen, "%s", probe_detail);
    pthread_mutex_unlock(&mu);
    return rc;
}

void super_set_local(int on)
{
    pthread_mutex_lock(&mu);
    int was = local_posture;
    local_posture = on ? 1 : 0;
    /* A posture change re-binds the Grbl port: the running controller
     * is stopped and the loop respawns it with the new address. */
    if (was != local_posture && child_pid > 0 && child_ctl == Ctl_Grbl) {
        fflog(LOG_NOTICE, "super: controller posture %s, restarting",
              local_posture ? "local" : "normal");
        stop_locked();
        respawn_at = 0.0;
    }
    pthread_mutex_unlock(&mu);
}

int super_gated(char *why, size_t len)
{
    pthread_mutex_lock(&mu);
    int g = gated;
    if (why && len)
        snprintf(why, len, "%s", gate_why);
    pthread_mutex_unlock(&mu);
    return g;
}
