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
 * probe runs once per broker hold.
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
 * scripts, manual start), the supervisor stands by rather than fight
 * over the exclusive-open pulse device and the Grbl port.
 */
#define _GNU_SOURCE
#include "cam.h"
#include "setup.h"
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "led.h"
#include "lenshome.h"
#include "liveness.h"
#include "paths.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "update.h"

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
 * at a directory of plain files. */
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
#define WR_ATTR_TRIES      3      /* a safing write is tried this often... */
#define WR_ATTR_RETRY_US   10000  /* ...this far apart */

typedef enum { Ctl_None = 0, Ctl_Grbl, Ctl_Cloud } ctl_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t th;
static int th_run = 0;

static ctl_t want = Ctl_None;      /* selected mode */
static int suspended = 0;          /* diag takeover / standby */
static pid_t child_pid = 0;
static ctl_t child_ctl = Ctl_None;
static double spawned_at = 0.0;
static unsigned backoff_s = RESPAWN_MIN_S;
static double respawn_at = 0.0;    /* not before this time */
static unsigned generation = 0;    /* bumped on every state change */

static int local_posture = 0;      /* a wizard owns the controller: loopback only */
static int gated = 0;              /* the setup gate is closed (under mu) */
static char gate_why[256];

static int broker_fd = -1;         /* /dev/glowforge, held for our lifetime */
static int broker_warned = 0;      /* the broker's refusal is said once per episode */
static int probed = 0;             /* liveness gate passed since broker open */
static int probe_skipped = 0;      /* ...but the probe itself could not run */
static int motion_fault = 0;       /* probe failed after recovery - no spawn */
static int probing = 0;            /* a probe sequence is in flight (under mu) */
static int probe_abort = 0;        /* ...and the stop lever asked it to end */
static int standby_takeover = 0;   /* unmanaged controller found: retake at idle */
static int enclosure_wait = 0;     /* the gate waits for the lid or the interlock */
static char wait_why[96];          /* ...and this is what is open */
static int runner_check = 0;       /* a GRBL reap: look for its homing runner */
static int respawn_wait_idle = 0;  /* the respawn waits for the kernel to finish */
static double died_at = 0.0;       /* ...counted from this unexpected death */
static int restart_pending = 0;    /* the engine asked for a stop and a respawn */
static int chld_efd = -1;          /* SIGCHLD lands here; the loop waits on it */

static int wr_attr(const char *attr, const char *val);

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Stop an unmanaged (legacy- or orphan-started) controller the legacy
 * way so the supervisor can take over. Called with mu NOT held. */
static void takeover_unmanaged(void)
{
    fflog(LOG_WARNING, "super: taking over from unmanaged "
                       "controller");
    (void)!system("/etc/init.d/grblhal stop >/dev/null 2>&1");
    (void)!system("/etc/init.d/gfcloud stop >/dev/null 2>&1");
    (void)!system("pkill -x grblHAL_glowfor 2>/dev/null");
    (void)!system("pkill -f '[g]fcloud\\.py' 2>/dev/null");
    (void)!system("pkill -x gfhome.py 2>/dev/null");
    sleep(1);
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
    (void)!system("pkill -TERM -x gfhome.py 2>/dev/null");
    double deadline = wall_s() + RUNNER_TERM_WAIT_S;
    while (runner_running() && wall_s() < deadline)
        usleep(100 * 1000);
    if (runner_running()) {
        fflog(LOG_WARNING, "super: escalating the homing runner to SIGKILL");
        (void)!system("pkill -KILL -x gfhome.py 2>/dev/null");
        deadline = wall_s() + 1.0;
        while (runner_running() && wall_s() < deadline)
            usleep(100 * 1000);
    }
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
    /* A just-stopped legacy holder's close can lag its exit. O_CLOEXEC:
     * only the controller spawn clears the flag before exec, so helper
     * children (curl, fwup, media-ctl, pkill, ...) can never pin the
     * pulse device and defeat the kernel dead-man on the description. */
    for (int i = 0; i < 30 && broker_fd < 0; i++) {
        broker_fd = open(PULSE_DEV, O_WRONLY | O_CLOEXEC);
        if (broker_fd < 0)
            usleep(100 * 1000);
    }
    /* No broker, no controller: a controller that opens the device
     * itself settles the rail on its own open (the one path that cycles
     * the 40 V), and a description without the lock has no dead-man.
     * The loop tries again; the refusal is said once per episode. */
    if (broker_fd < 0) {
        if (!broker_warned)
            fflog(LOG_CRIT, "super: cannot open " PULSE_DEV ": %s - no "
                            "controller until the broker holds it",
                  strerror(errno));
        broker_warned = 1;
        return;
    }
    if (flock(broker_fd, LOCK_EX) != 0) {
        if (!broker_warned)
            fflog(LOG_CRIT, "super: flock on " PULSE_DEV " failed: %s - the "
                            "kernel dead-man would be unarmed; no controller "
                            "until it locks", strerror(errno));
        broker_warned = 1;
        close(broker_fd);
        broker_fd = -1;
        return;
    }
    broker_warned = 0;
    probed = 0;     /* a fresh hold means unverified motion */
    probe_skipped = 0;
    lenshome_clear();   /* ...and an unreferenced lens */
    fflog(LOG_INFO, "super: holding " PULSE_DEV " (broker)");
}

/* The gate's wait for the enclosure. The probe moves the gantry, so it
 * does not run while a lid or the interlock is open, or while the
 * switches cannot be read: no controller until the enclosure closes,
 * /mode says "waiting" and why, and the button blinks amber so the
 * machine itself asks for the lid. Called with mu held. */
static void wait_enter_locked(const char *why)
{
    if (enclosure_wait && strcmp(why, wait_why) == 0)
        return;
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

/* Motion-liveness gate, run with mu NOT held (takes seconds; the
 * recovery ladder takes a minute). The DRV8825 drivers can come out of
 * a rail power-up unserviceable; each recovery attempt gives them a
 * longer true power-off before re-probing. Returns 1 verified, 0 fault,
 * 2 when the probe could not run (no accelerometer): the machine is not
 * blocked, but motion stays UNVERIFIED and is reported so; 3 when the
 * probe must wait and run again - the enclosure opened between the
 * gate's own check and the move, or the kernel is still playing the
 * last program: nothing moved, nothing is decided; 4 when the stop
 * lever ended it. The fans are not quieted for the probe: its verdict
 * is peak-to-peak over a commanded move, and the moving and dead
 * thresholds sit twice away from what the bench reference reads with
 * its fans at their idle duty. */
static char probe_detail[96];       /* the last probe's outcome text */
static int probe_sequence(int fd)
{
    static const int ladder_s[] = { 0, 5, 15, 30 };
    char *detail = probe_detail;

    for (size_t i = 0; i < sizeof(ladder_s) / sizeof(ladder_s[0]); i++) {
        if (ladder_s[i] > 0) {
            fflog(LOG_WARNING, "super: motion dead - rail off %d s "
                               "and re-probing (%zu/3)", ladder_s[i], i);
            wr_attr("cnc/disable", "1");
            if (probe_wait((unsigned)ladder_s[i]))
                goto aborted;
        }
        if (probe_aborted())
            goto aborted;
        wr_attr("cnc/enable", "1");
        if (probe_wait(1))
            goto aborted;
        int rc = liveness_probe(fd, detail, sizeof(probe_detail));
        fflog(rc == -3 ? LOG_NOTICE : LOG_INFO, "super: liveness probe: %s - %s",
              rc == 1 ? "MOTION OK" : rc == 0 ? "NO MOTION"
              : rc == -3 ? "WAITS" : "ERROR", detail);
        if (rc == -3)
            return 3;   /* the kernel is busy: wait for it, then probe */
        if (rc == 1) {
            /* The gantry moves; now the lens takes its one reference,
             * while the machine is still ours. A lens that cannot reach
             * its hall edge is broken hardware, so it gates the spawn the
             * same way dead drivers do: the focal height would otherwise
             * be a guess. */
            /* Sized to leave room for the prefix below inside
             * probe_detail, which this shares with the probe. */
            char lens[80];
            int lrc = lenshome_run(lens, sizeof(lens));
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
            return 3;   /* the enclosure opened under the probe: wait for it */
        if (rc < 0)
            return 2;   /* cannot probe (no accel?): do not block the machine */
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

/* The safing writes (cnc/stop, cnc/laser_latch) are the real safing
 * mechanism under the device broker: a write that fails is tried again
 * before it is named at the highest severity, and the callers on the
 * safing paths repeat the pair once more. Returns 0 when the value
 * landed. */
static int wr_attr(const char *attr, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    int err = 0;
    for (int i = 0; i < WR_ATTR_TRIES; i++) {
        if (i)
            usleep(WR_ATTR_RETRY_US);
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            err = errno;
            continue;
        }
        ssize_t w = write(fd, val, strlen(val));
        err = w < 0 ? errno : 0;
        close(fd);
        if (!err)
            return 0;
    }
    fflog(LOG_CRIT, "super: writing %s to %s failed %d times: %s", val, attr,
          WR_ATTR_TRIES, strerror(err));
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
static char **build_child_env(ctl_t ctl, int pulse_fd)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = calloc(n + 3, sizeof(*env));
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        /* Our own entries replace any inherited value of the same key. */
        if (!strncmp(environ[i], "GF_PULSE_FD=", 12) ||
            !strncmp(environ[i], "GFSINK=", 7))
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
    if (ctl == Ctl_Grbl) {
        if (!(env[k] = strdup("GFSINK=" PULSE_DEV)))
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

/* A controller's stray stdout/stderr - interpreter tracebacks, library
 * messages, anything not sent through its own syslog emitter - flows
 * through a `logger` relay into syslog under the controller's program
 * name, so it lands in that controller's log directory. The relay is
 * double-forked (init reaps it) and lives as long as the pipe has a
 * writer, so it outlives this daemon whenever the controller does.
 * Returns the pipe's write end, or -1 (the controller then runs with
 * its output discarded rather than not at all). */
static int spawn_output_relay(const char *tag)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        pid_t g = fork();
        if (g != 0)
            _exit(g < 0 ? 127 : 0);
        dup2(p[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
            maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            close(fd);
        /* Reclaimed no sooner than the controller it serves: a dead
         * relay turns the controller's stray writes into EPIPE. */
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
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

static void spawn_locked(ctl_t ctl)
{
    broker_open_locked();

    const char *data_dir = ff_data_dir();
    char nvs[256] = "";
    if (ctl == Ctl_Grbl)
        grbl_nvs_path(nvs, sizeof nvs);

    char **env = build_child_env(ctl, broker_fd);
    if (!env) {
        fflog(LOG_ERR, "super: cannot build the controller "
                       "environment: %s", strerror(errno));
        respawn_at = wall_s() + backoff_s;
        return;
    }

    int lfd = spawn_output_relay(ctl == Ctl_Grbl ? "grblhal" : "gfcloud");
    pid_t pid = fork();
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
        /* Child: async-signal-safe calls only from here to exec. Own
         * process group so stop() can signal helpers too. */
        setpgid(0, 0);
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
         * pinned for the whole life of the child. */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
            maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            if (fd != broker_fd)
                close(fd);
        /* A controller must be a less-preferred OOM victim than
         * ordinary processes, but a MORE preferred one than the daemon
         * (which is its dead-man and respawns it). */
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
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
                execle(GRBL_BIN, GRBL_BIN, "-p", "23", "-b", "::1",
                       "-e", nvs, (char *)NULL, env);
            else
                execle(GRBL_BIN, GRBL_BIN, "-p", "23",
                       "-e", nvs, (char *)NULL, env);
        } else {
            execle(CLOUD_BIN, CLOUD_BIN, (char *)NULL, env);
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
    child_pid = 0;
    child_ctl = Ctl_None;
    generation++;
    pthread_cond_broadcast(&cv);

    int expected = suspended || want != died || restart_pending;
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
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");
    /* The homing anchor belongs to the controller that wrote it: it
     * must not survive into another mode (cloud re-zeros the counters
     * it anchors) or into a respawn. A fresh GRBL controller starts
     * unreferenced and re-homes. */
    unlink(HOMED_ANCHOR);
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
    if (backoff_s < RESPAWN_MAX_S)
        backoff_s *= 2;
}

/* Stop the child: SIGTERM its group, escalate to SIGKILL. Called with
 * mu held; drops and retakes the lock while waiting (the thread's
 * waitpid runs reap_locked). */
static void stop_locked(void)
{
    if (child_pid <= 0)
        return;
    pid_t pid = child_pid;
    fflog(LOG_INFO, "super: stopping %s controller (pid %d)",
          ctl_name(child_ctl), (int)pid);
    /* Safe the machine BEFORE the signal, not only after the reap: this
     * path is also the emergency lever (POST /controller/stop is not
     * idle-gated), and a controller that is mid-job may take its whole
     * SIGTERM grace to leave. cnc/stop is a controlled deceleration and
     * the latch relock severs FIRE - both instantaneous at the kernel,
     * both harmless no-ops when the machine is already idle and latched. */
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    /* Once more after the signal: a write that failed its retries the
     * first time gets a second chance while the controller leaves. */
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");
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
             * the machine now rather than waiting for the reap. */
            wr_attr("cnc/stop", "1");
            wr_attr("cnc/laser_latch", "1");
            deadline = wall_s() + STOP_TERM_WAIT_S;
        }
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
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
        /* Standing by next to an unmanaged controller (found at start,
         * or left orphaned by a previous forgectrl): retake supervision
         * as soon as the machine is idle - a busy one is left to finish
         * its job first. */
        if (standby_takeover && child_pid == 0 && want != Ctl_None) {
            pthread_mutex_unlock(&mu);
            int idle = machine_is_idle() && !diag_running();
            if (idle)
                takeover_unmanaged();
            pthread_mutex_lock(&mu);
            if (idle) {
                standby_takeover = 0;
                suspended = 0;
            }
        }

        /* The setup gate: no controller for a sender while it
         * is closed. A wizard's own controller (local posture, loopback
         * only) passes. A gate that closes on a running controller (a
         * required re-run raised mid-life) stops it once the machine
         * is idle, never mid-job. */
        {
            char why[256];
            pthread_mutex_unlock(&mu);
            int open = setup_gate_open(why, sizeof(why));
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
         * the probe and its recovery ladder take a while). */
        if (child_pid > 0 && (suspended || child_ctl != want)) {
            stop_locked();
        } else if (child_pid == 0 && !suspended && !motion_fault && !gated
                   && want != Ctl_None && wall_s() >= respawn_at) {
            int go = 1;
            if (respawn_wait_idle) {
                /* The kernel finishes what the dead controller queued
                 * before another controller may touch it. A kernel
                 * that has not gone idle in the bound is halted: an
                 * endless ramp is not a state to hand over. */
                pthread_mutex_unlock(&mu);
                int idle = machine_is_idle();
                pthread_mutex_lock(&mu);
                if (idle)
                    respawn_wait_idle = 0;
                else if (wall_s() - died_at < RESPAWN_IDLE_WAIT_S)
                    go = 0;
                else {
                    fflog(LOG_CRIT, "super: the kernel is still running %d s "
                                    "after the controller left - halting it "
                                    "before the respawn", RESPAWN_IDLE_WAIT_S);
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
                    if (enc != 0) {
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

/* The bracketed patterns keep pgrep/pkill -f from matching their own
 * sh -c wrapper. */
static int unmanaged_controller_running(void)
{
    return system("pgrep -x grblHAL_glowfor >/dev/null 2>&1") == 0 ||
           system("pgrep -f '[g]fcloud\\.py' >/dev/null 2>&1") == 0 ||
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

void super_init(void)
{
    super_sigchld_init();
    pthread_mutex_lock(&mu);
    want = configured_mode();
    if (unmanaged_controller_running()) {
        suspended = 1;
        standby_takeover = 1;
        fflog(LOG_WARNING, "super: unmanaged controller already "
                           "running - standing by until the machine is idle");
    }
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

void super_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = th_run;
    th_run = 0;
    suspended = 1;
    wait_leave_locked("no longer waits: shutdown");
    if (child_pid > 0) {
        /* A busy controller survives a forgectrl stop: it reparents to
         * init and finishes its job (its own device fd carries the
         * dead-man). The restarted supervisor finds it unmanaged and
         * stands by. Idle controllers stop with us. */
        if (machine_is_idle())
            stop_locked();
        else
            fflog(LOG_WARNING, "super: machine busy - leaving %s "
                               "controller running (pid %d, unmanaged)",
                  ctl_name(child_ctl), (int)child_pid);
    }
    /* Our reference goes away either way. Idle: the device closes and
     * the kernel locks the latch. Busy-orphan: the child's dup keeps
     * the description open (job survives), and the child's own exit
     * then IS the final close - the kernel dead-man backstop. */
    if (broker_fd >= 0) {
        close(broker_fd);
        broker_fd = -1;
    }
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(th, NULL);
}

int super_mode_switch(const char *mode, char *err, size_t elen)
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
        if (!setup_gate_open(why, sizeof(why)) && !local_posture) {
            snprintf(err, elen, "controllers are gated: %s", why);
            return -1;
        }
    }

    if (diag_running()) {
        snprintf(err, elen, "a diagnostic is running");
        return -1;
    }
    if (update_job_running()) {
        snprintf(err, elen, "an update job is running");
        return -1;
    }
    if (!machine_is_idle()) {
        snprintf(err, elen, "machine is not idle");
        return -1;
    }

    pthread_mutex_lock(&mu);
    int takeover = suspended && unmanaged_controller_running();
    pthread_mutex_unlock(&mu);
    if (takeover)
        takeover_unmanaged();

    if (settings_set("controller_mode", mode) != 0) {
        snprintf(err, elen, "cannot persist controller_mode");
        return -1;
    }

    pthread_mutex_lock(&mu);
    want = target;
    suspended = 0;
    standby_takeover = 0;
    motion_fault = 0;   /* a switch is also the retry lever after a fault */
    /* The thread stops the old controller and starts the new one; wait
     * until the running child IS the target. A pending liveness
     * (re-)probe runs first, so allow for it. */
    double sw_deadline = wall_s() + 90.0;
    while (!(child_pid > 0 && child_ctl == target)) {
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

int super_controller_stop(void)
{
    pthread_mutex_lock(&mu);
    suspended = 1;
    /* A probe in flight is the gantry moving with no controller: the
     * lever reaches it too. The sequence looks between its steps. */
    if (probing) {
        probe_abort = 1;
        wr_attr("cnc/stop", "1");
    }
    int ok = wait_for(pred_child_gone, 15.0) == 0;
    if (!ok)
        suspended = 0;  /* takeover failed: resume normal supervision so
                         * the machine is never left controller-less */
    pthread_mutex_unlock(&mu);
    return ok ? 0 : -1;
}

void super_controller_start(void)
{
    pthread_mutex_lock(&mu);
    suspended = 0;
    motion_fault = 0;
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
    pthread_mutex_lock(&mu);
    int up = child_pid > 0 && strcmp(ctl_name(child_ctl), "grbl") == 0;
    pthread_mutex_unlock(&mu);
    return up;
}

int super_status_json(char *buf, size_t len)
{
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
             "\"motion\":\"%s\",\"gated\":%s,\"local\":%s,\"why\":\"%s\"}",
             ctl_name(want),
             child_pid > 0 ? "running"
                 : gated ? "gated"
                 : motion_fault ? "motion-fault"
                 : enclosure_wait ? "waiting"
                 : suspended ? "standby" : "stopped",
             (int)child_pid,
             motion_fault ? "fault"
                 : probed && !probe_skipped ? "verified" : "unverified",
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
    broker_open_locked();
    int fd = broker_fd;
    if (fd < 0) {
        pthread_mutex_unlock(&mu);
        snprintf(detail, dlen, "the pulse device is not held");
        return 2;
    }
    probing = 1;
    probe_abort = 0;
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
