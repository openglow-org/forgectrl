/*
 * enclosure_wait_test.c - host unit test for the motion gate's enclosure read
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The motion-liveness probe moves the gantry, so the supervisor's gate
 * runs it only with the lid and the interlock closed, and waits for them
 * otherwise (no controller, /mode "waiting", the button amber). What is
 * provable on a host: the EV_SW word maps to the right verdict and the
 * right words for the operator, and every way the switch read can fail
 * (a missing device, a path that is not an input device) lands on
 * "open", never on "closed". The switch word from a file, the host
 * start's seam GF_SWITCH_WORD_FILE, is read by an absolute path only:
 * the word the file holds classifies as the device's would, and a
 * relative path is a failed read, so "open".
 *
 * The test drives liveness_enclosure() from liveness.c through the
 * GF_SWITCH_DEV and GF_SWITCH_WORD_FILE seams.
 *
 * The probe itself, liveness_probe(), runs here too: liveness.c is
 * included over a fake clock, its iio directory moved (GF_IIO_ROOT) to a
 * scratch tree that stands in for the head accelerometer, its sysfs tree
 * to another (GF_SYSFS_ROOT), and a plain file for the pulse device. What
 * is pinned:
 * a kernel still playing, and an enclosure open before the move, write
 * nothing; the caller's stop, and a lid that opens during the current
 * settle, are asked just before the run, which then never starts; a
 * setup write that does not land is a fault of its own that
 * names the write, with nothing run; a move with no accelerometer sample,
 * and a hold current the PIC refuses after the move, are that fault too;
 * switches that stop answering under the move stop it, and the probe says
 * the head stands where the stop caught it; a still head reads no motion,
 * with the setup written, the run started, and the hold current put back.
 */
#define _GNU_SOURCE
#define clock_gettime fake_clock_gettime
#define usleep fake_usleep

#include <time.h>

int fake_clock_gettime(clockid_t id, struct timespec *ts);
int fake_usleep(unsigned us);

#include "../src/liveness.c"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef clock_gettime
#undef usleep

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* EV_SW bits: 3 = doors (set = closed), 5 = interlock (set = open). */
#define DOORS_CLOSED   (1u << 3)
#define INTERLOCK_OPEN (1u << 5)

/* --- the probe's machine ---------------------------------------------- */

#define T "liveness-test"
static double fake_now = 1000.0;
static char switch_file[512];           /* the switch word, by its absolute path */
static int open_at_run;                 /* the switches stop answering once the run is started */
static int pic_gone_at_run;             /* the PIC takes no write once the run is started */
static int open_at_settle;              /* the lid opens during the current settle */
static int lever;                       /* the caller's stop input answers */

static int lever_stop(void)
{
    return lever;
}

static void put(const char *rel, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), T "/%s", rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void get(const char *rel, char *buf, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), T "/%s", rel);
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(buf, (int)len, f))
            buf[0] = '\0';
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = '\0';
}

static void put_switches(const char *word)
{
    FILE *f = fopen(switch_file, "w");
    if (f) {
        fputs(word, f);
        fclose(f);
    }
}

/* Each reading moves the clock on, so the probe's waits end; and once the
 * run is started, the lid opens when the case asks for it. */
int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    fake_now += 0.05;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    if (open_at_run || pic_gone_at_run) {
        char run[8];
        get("sys/cnc/run", run, sizeof(run));
        if (!strcmp(run, "1")) {
            if (open_at_run)
                put_switches("eight\n");        /* a word the read refuses: not closed */
            if (pic_gone_at_run)
                unlink(T "/sys/pic/x_step_current");
            open_at_run = pic_gone_at_run = 0;
        }
    }
    return 0;
}

int fake_usleep(unsigned us)
{
    fake_now += us / 1e6;
    if (open_at_settle) {
        put_switches("0\n");
        open_at_settle = 0;
    }
    return 0;
}

/* The kernel idle and every attribute the probe writes empty; the head
 * accelerometer still; the enclosure closed; the pulse device empty. */
static void machine(void)
{
    static const char *const attrs[] = { "cnc/motor_lock", "cnc/x_mode", "cnc/x_decay", "pic/x_step_current",
                                         "cnc/step_freq", "cnc/streaming", "cnc/run", "cnc/stop" };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(*attrs); i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "sys/%s", attrs[i]);
        put(rel, "");
    }
    put("sys/cnc/state", "idle\n");
    put("dev/3-001e/iio:device0/in_accel_x_raw", "12\n");
    put("dev/3-001e/iio:device0/in_accel_y_raw", "-30\n");
    put("pulse", "");
    put_switches("8\n");
    open_at_run = pic_gone_at_run = open_at_settle = lever = 0;
}

static int probe(char *detail, size_t dlen)
{
    int fd = open(T "/pulse", O_RDWR | O_CLOEXEC);
    int rc = liveness_probe(fd, detail, dlen, lever_stop);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int written(const char *rel)
{
    char v[32];
    get(rel, v, sizeof(v));
    return v[0] != '\0';
}

static long pulse_bytes(void)
{
    struct stat st;
    return stat(T "/pulse", &st) == 0 ? (long)st.st_size : -1;
}

int main(void)
{
    char why[96];

    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
    printf("the classification of the switch word\n");
    memset(why, 'x', sizeof(why));
    CHECK(liveness_enclosure_classify(DOORS_CLOSED, why, sizeof(why)) == 0 && why[0] == '\0',
          "doors closed, interlock satisfied: closed, no words");
    CHECK(liveness_enclosure_classify(0, why, sizeof(why)) == 1 && !strcmp(why, "the lid is open"),
          "doors open: the lid is open");
    CHECK(liveness_enclosure_classify(DOORS_CLOSED | INTERLOCK_OPEN, why, sizeof(why)) == 1
          && !strcmp(why, "the interlock is open"),
          "doors closed, interlock open: the interlock is open");
    CHECK(liveness_enclosure_classify(INTERLOCK_OPEN, why, sizeof(why)) == 1
          && !strcmp(why, "the lid and the interlock are open"),
          "both open: named together");
    CHECK(liveness_enclosure_classify(0, why, 1) == 1 && why[0] == '\0',
          "a one-byte why buffer is terminated");

    printf("the read fails open\n");
    setenv("GF_SWITCH_DEV", "/nonexistent/input/event0", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a missing switch device reads as open, with the reason");
    setenv("GF_SWITCH_DEV", "/dev/null", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a path that is not an input device reads as open");

    printf("the switch word from a file (GF_SWITCH_WORD_FILE)\n");
    char wf[] = "/tmp/enclosure-wait-XXXXXX";
    int fd = mkstemp(wf);
    CHECK(fd >= 0 && write(fd, "8\n", 2) == 2, "a word file can be made");
    close(fd);
    setenv("GF_SWITCH_WORD_FILE", wf, 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == 0, "8 (doors closed, loop closed): closed, the device unread");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "0x28\n", 5); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == 1 && !strcmp(why, "the interlock is open"),
          "0x28: the interlock is open");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "0\n", 2); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == 1 && !strcmp(why, "the lid is open"), "0: the lid is open");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "eight\n", 6); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a word that is no integer reads as open");
    FILE *rel = fopen("enclosure-wait-word", "w");
    if (rel) {
        fputs("8\n", rel);
        fclose(rel);
    }
    setenv("GF_SWITCH_WORD_FILE", "enclosure-wait-word", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "the same word by a relative path is refused by its form: open");
    unlink("enclosure-wait-word");
    unlink(wf);
    unsetenv("GF_SWITCH_WORD_FILE");

    printf("the probe\n");
    if (system("rm -rf " T) != 0)
        failures++;
    mkdir(T, 0755);
    mkdir(T "/sys", 0755);
    mkdir(T "/sys/cnc", 0755);
    mkdir(T "/sys/pic", 0755);
    mkdir(T "/dev", 0755);
    mkdir(T "/dev/3-001e", 0755);
    mkdir(T "/dev/3-001e/iio:device0", 0755);
    mkdir(T "/iio", 0755);
    CHECK(symlink("../dev/3-001e/iio:device0", T "/iio/iio:device0") == 0, "the head accelerometer stands in");
    char cwd[384];
    snprintf(switch_file, sizeof(switch_file), "%s/" T "/switches", getcwd(cwd, sizeof(cwd)) ? cwd : ".");
    setenv("GF_SWITCH_WORD_FILE", switch_file, 1);
    setenv("GF_SYSFS_ROOT", T "/sys/", 1);
    setenv("GF_IIO_ROOT", T "/iio", 1);
    char detail[96], v[32];

    machine();
    put("sys/cnc/state", "running\n");
    CHECK(probe(detail, sizeof(detail)) == -3 && !written("sys/cnc/motor_lock") && pulse_bytes() == 0,
          "a kernel still playing: the probe waits, nothing written");
    machine();
    put_switches("0\n");
    CHECK(probe(detail, sizeof(detail)) == -2 && !written("sys/cnc/motor_lock") && pulse_bytes() == 0,
          "the lid open before the move: the probe waits, nothing written");
    machine();
    unlink(T "/sys/pic/x_step_current");    /* the PIC takes no write */
    int rc = probe(detail, sizeof(detail));
    CHECK(rc == -4 && strstr(detail, "pic/x_step_current") && !written("sys/cnc/run") && pulse_bytes() == 0,
          "a setup write that does not land: a fault naming it, nothing run");
    machine();
    lever = 1;                              /* the stop lever, during the current settle */
    CHECK(probe(detail, sizeof(detail)) == -6 && !written("sys/cnc/run"),
          "the caller's stop before the move: -6, the run never started");
    machine();
    open_at_settle = 1;
    CHECK(probe(detail, sizeof(detail)) == -2 && !written("sys/cnc/run"),
          "the lid opening during the settle: -2, the run never started");
    machine();
    unlink(T "/dev/3-001e/iio:device0/in_accel_x_raw");
    CHECK(probe(detail, sizeof(detail)) == -4 && written("sys/cnc/run"),
          "a move with no accelerometer sample: the probe's fault, not a skip");
    machine();
    open_at_run = 1;
    rc = probe(detail, sizeof(detail));
    get("sys/cnc/stop", v, sizeof(v));
    CHECK(rc == -5 && !strcmp(v, "1"), "switches that stop answering under the move stop it: -5, the head where it stood");
    machine();
    pic_gone_at_run = 1;
    rc = probe(detail, sizeof(detail));
    CHECK(rc == -4 && strstr(detail, "pic/x_step_current"), "a hold current the PIC refuses after the move: the probe's fault");
    machine();
    rc = probe(detail, sizeof(detail));
    char lock[8], mode[8], cur[8];
    get("sys/cnc/motor_lock", lock, sizeof(lock));
    get("sys/cnc/x_mode", mode, sizeof(mode));
    get("sys/pic/x_step_current", cur, sizeof(cur));
    get("sys/cnc/run", v, sizeof(v));
    /* A plain file keeps the tail of the longer write before it: the
     * stand-down's 33 lands over the run current's 135 as 335. */
    CHECK(rc == 0 && !strcmp(lock, "0") && !strcmp(mode, "8") && !strcmp(v, "1") && !strncmp(cur, "33", 2) &&
          pulse_bytes() > 0, "a still head: the setup written, the run started, no motion, the hold current back");
    unsetenv("GF_SWITCH_WORD_FILE");
    unsetenv("GF_SYSFS_ROOT");
    unsetenv("GF_IIO_ROOT");
    if (system("rm -rf " T) != 0)
        failures++;

    printf(failures ? "enclosure_wait_test: %d FAILED\n" : "enclosure_wait_test: all ok\n", failures);
    return failures ? 1 : 0;
}
