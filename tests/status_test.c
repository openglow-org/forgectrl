/*
 * status_test.c - host unit test for the machine status reader
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The real status.c, pointed at one temp directory through its test
 * seams (GF_SYSFS_ROOT, GF_SWITCH_DEV, GF_PROC_STAT, GF_RUN_DIR,
 * FORGECTRL_DATA_DIR), with the supervisor stubbed. Four parts:
 *
 * The camera privacy gate. Every camera entry point in cam.c refuses
 * while machine_lid_closed() is false, but for the head camera to a
 * local viewer, so a read that failed open would let the sensors
 * capture, and in cloud mode upload, with the enclosure open. The
 * positive direction needs the real switch and is the bench's
 * (camera.lid-privacy). Every way the read can fail lands on not closed:
 * the device missing, a path that is no input device, a device without
 * EVIOCGSW, and the fd table exhausted under a connection flood. The
 * reader keeps one fd across calls, so the failures repeat in one
 * process: none may read closed, none may leave an fd behind. The reader
 * caches its device path on first use, so each case runs in its own
 * child, and this part runs before the parent reads the switch at all.
 *
 * machine_is_idle() fails closed: not idle on any read failure, the
 * fd-table exhaustion a connection flood can cause (EMFILE) included. A
 * read that failed open would let a destructive action (a flash, a mode
 * switch, a diagnostic) or a mid-cut safing drop proceed on a busy
 * machine.
 *
 * The sys block. The /status document carries CPU and memory use next
 * to the board temperatures. The CPU number is a delta over the interval
 * since the previous read, so the first read reports null and a later
 * one a percent; memory reports at once. The whole document fits the
 * handler's buffer and stays intact. The sampler reads a stat file of
 * the test's own; this part's first read is the sampler's first, so it
 * runs before any other part renders the document.
 *
 * The grbl block. The controller publishes its state to
 * <run dir>/grbl.state; /status echoes it as
 * "grbl":{"age_s":...,"report":{...}} only while the supervisor holds a
 * live GRBL controller. A dead controller, an absent file, a torn body
 * (no closing brace) or an absurd age all read as no block at all: the
 * panel treats absence as "no controller", never as stale truth. The
 * lens block's reach moves up by the crumb tray's offset while the
 * controller's tray.out marker stands.
 */
#define _GNU_SOURCE
#include "../src/status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* status.c's only external dependencies. */
int diag_running(void) { return 0; }
/* No settings store on the host: the lens block reads its defaults. */
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }
static int grbl_up;
int super_grbl_running(void) { return grbl_up; }

static int failures;
static char dir[256];        /* the temp directory */
static char root[260];       /* GF_SYSFS_ROOT: the same, with a trailing '/' */

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* --- the camera privacy gate ------------------------------------------ */

/* machine_lid_closed() caches the device path on first use, so each case
 * runs in its own process. Returns the child's verdict. */
static int lid_closed_with(const char *dev)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("GF_SWITCH_DEV", dev, 1);
        _exit(machine_lid_closed() ? 1 : 0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        fprintf(stderr, "child did not exit normally\n");
        exit(2);
    }
    return WEXITSTATUS(st);
}

/* Same, with the fd table exhausted first so open() fails with EMFILE. */
static int lid_closed_under_emfile(const char *dev)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("GF_SWITCH_DEV", dev, 1);
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        rl.rlim_cur = 64;
        setrlimit(RLIMIT_NOFILE, &rl);
        int n = 0;
        while (n < 64 && open("/dev/null", O_RDONLY) >= 0)
            n++;
        int closed = machine_lid_closed();
        /* 2 = the fd table was never actually exhausted (inconclusive) */
        _exit(errno != EMFILE ? 2 : (closed ? 1 : 0));
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        fprintf(stderr, "child did not exit normally\n");
        exit(2);
    }
    return WEXITSTATUS(st);
}

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

/* The reader keeps one fd across calls, and only after a read succeeds:
 * a hundred failing reads in one process must each report NOT closed and
 * leave the fd table as they found it. 0 = held, 1 = a read reported
 * closed, 3 = the fd count moved. */
static int lid_repeated_with(const char *dev)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("GF_SWITCH_DEV", dev, 1);
        machine_lid_closed();
        int before = count_fds();
        for (int i = 0; i < 100; i++)
            if (machine_lid_closed())
                _exit(1);
        _exit(before < 0 || count_fds() != before ? 3 : 0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        fprintf(stderr, "child did not exit normally\n");
        exit(2);
    }
    return WEXITSTATUS(st);
}

static void lid_gate(void)
{
    char missing[320], regular[320];
    snprintf(missing, sizeof(missing), "%s/no-such-device", dir);
    snprintf(regular, sizeof(regular), "%s/not-an-input-device", dir);
    FILE *f = fopen(regular, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs("this is not an evdev node", f);
    fclose(f);

    printf("machine_lid_closed() fail-closed (camera privacy gate):\n");

    CHECK(lid_closed_with(missing) == 0,
          "reports NOT closed when the switch device is missing");
    CHECK(lid_closed_with(regular) == 0,
          "reports NOT closed when the path is not an input device");
    CHECK(lid_closed_with("/dev/null") == 0,
          "reports NOT closed when EVIOCGSW is unsupported");

    int emfile = lid_closed_under_emfile(regular);
    CHECK(emfile != 2, "fd table was actually exhausted (EMFILE)");
    CHECK(emfile == 0, "reports NOT closed under EMFILE");

    CHECK(lid_repeated_with(missing) == 0,
          "100 reads of a missing device: each NOT closed, no fd kept");
    CHECK(lid_repeated_with(regular) == 0,
          "100 reads of a non-input path: each NOT closed, no fd kept");
    CHECK(lid_repeated_with("/dev/null") == 0,
          "100 reads without EVIOCGSW: each NOT closed, no fd kept");

    unlink(regular);
}

/* --- machine_is_idle() -------------------------------------------------- */

static void set_state(const char *val)
{
    char path[320];
    snprintf(path, sizeof(path), "%scnc", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%scnc/state", root);
    if (val) {
        FILE *f = fopen(path, "w");
        if (!f) { perror("fopen state"); exit(2); }
        fputs(val, f);
        fclose(f);
    } else {
        unlink(path);
    }
}

static void idle_fails_closed(void)
{
    printf("machine_is_idle() fail-closed:\n");

    set_state("idle");
    CHECK(machine_is_idle() == 1, "reads idle when cnc/state is \"idle\"");

    set_state("running");
    CHECK(machine_is_idle() == 0, "reads busy when cnc/state is \"running\"");

    set_state("idle\n");
    CHECK(machine_is_idle() == 1, "trailing newline still reads idle");

    /* "disabled" is the power-on state, and the state a machine holds
     * for the whole of its first run (the steppers are energized by the
     * liveness probe, which runs at a controller spawn, which the
     * setup gate holds back). Steppers off with no program in
     * progress is idle for every caller of this predicate; reading it
     * as busy locked the setup out of its own machine. */
    set_state("disabled");
    CHECK(machine_is_idle() == 1, "reads idle when cnc/state is \"disabled\"");
    set_state("disabled\n");
    CHECK(machine_is_idle() == 1, "trailing newline still reads disabled as idle");

    /* The two states that need an acknowledgment before the machine is
     * anyone's to take stay busy: a driver fault needs an enable, and an
     * underrun leaves the position untrusted until it is acknowledged. */
    set_state("fault");
    CHECK(machine_is_idle() == 0, "reads busy when cnc/state is \"fault\"");
    set_state("underrun");
    CHECK(machine_is_idle() == 0, "reads busy when cnc/state is \"underrun\"");

    set_state(NULL);
    CHECK(machine_is_idle() == 0, "fails closed when cnc/state is missing");

    /* EMFILE: with a valid "idle" state present, exhaust the fd table so
     * rd_attr's open() fails with EMFILE - machine_is_idle must still
     * report busy, not idle. */
    set_state("idle");
    CHECK(machine_is_idle() == 1, "reads idle again before fd exhaustion");
    struct rlimit rl, saved;
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = 64;                    /* small: quick to exhaust */
    setrlimit(RLIMIT_NOFILE, &rl);
    int held[64];
    int n = 0;
    while (n < 64) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        held[n++] = fd;
    }
    int idle_under_emfile = machine_is_idle();
    for (int i = 0; i < n; i++)
        close(held[i]);
    setrlimit(RLIMIT_NOFILE, &saved);
    CHECK(n > 0 && errno == EMFILE, "fd table was actually exhausted (EMFILE)");
    CHECK(idle_under_emfile == 0,
          "fails closed to busy under EMFILE with an \"idle\" state present");

    /* The tree empty again for the parts after this one. */
    set_state(NULL);
    char sub[320];
    snprintf(sub, sizeof(sub), "%scnc", root);
    rmdir(sub);
}

/* --- the sys block ------------------------------------------------------ */

/* The numeric value following "key": in the document, or -1000 when the
 * key is absent or its value is null. */
static double json_num(const char *doc, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(doc, pat);
    if (!p)
        return -1000;
    p += strlen(pat);
    if (strncmp(p, "null", 4) == 0)
        return -1000;
    return strtod(p, NULL);
}

static int json_is_null(const char *doc, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(doc, pat);
    return p && strncmp(p + strlen(pat), "null", 4) == 0;
}

/* One stat file for the sampler, rewritten in place. */
static void write_stat(const char *path, const char *line)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("stat file"); exit(2); }
    fputs(line, f);
    fclose(f);
}

/* Structural sanity: braces balance and the document closes itself. */
static int json_intact(const char *doc)
{
    int depth = 0;
    for (const char *p = doc; *p; p++) {
        if (*p == '{') depth++;
        if (*p == '}') depth--;
    }
    size_t n = strlen(doc);
    return depth == 0 && n > 2 && doc[0] == '{' && doc[n - 1] == '}';
}

static void sys_block(void)
{
    setenv("GF_SWITCH_DEV", "/nonexistent/forgectrl-switch", 1);

    printf("machine status JSON sys block:\n");
    char doc[3072];               /* the /status handler's body size, main.c */

    /* The sampler over a stat file the test writes
     * (cpu user nice system idle iowait irq softirq steal). */
    char statf[300];
    snprintf(statf, sizeof(statf), "%s/stat", dir);
    setenv("GF_PROC_STAT", statf, 1);
    write_stat(statf, "cpu 10 0 10 70 10 0 0 0\n");           /* total 100, idle 80 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0,
          "first status document renders");
    CHECK(json_intact(doc), "first document is structurally intact");
    CHECK(strstr(doc, "\"sys\":{\"cpu_pct\":") != NULL, "sys block present");
    CHECK(json_is_null(doc, "cpu_pct"), "first read reports cpu_pct null (unprimed)");
    double mem = json_num(doc, "mem_pct");
    CHECK(mem > 0 && mem < 100, "mem_pct is a percent on the first read");
    write_stat(statf, "cpu 15 0 15 160 10 0 0 0\n");          /* total +100, idle +90 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "a later tick renders");
    CHECK(json_intact(doc), "the later document is structurally intact");
    CHECK(json_num(doc, "cpu_pct") == 10.0, "the interval's busy share is the percent (10.0)");
    unlink(statf);
    unsetenv("GF_PROC_STAT");

    /* The document still ends with the switch map: nothing after the
     * sys block was pushed out of the buffer. */
    CHECK(strstr(doc, "\"hv_enable\":") != NULL, "switch map survives (no truncation)");
}

/* --- the grbl block and the lens block ---------------------------------- */

static void put_state(const char *fmt, double ts)
{
    char path[320], body[512];
    snprintf(path, sizeof(path), "%s/grbl.state", dir);
    snprintf(body, sizeof(body), fmt, ts);
    FILE *f = fopen(path, "w");
    fputs(body, f);
    fclose(f);
}

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void grbl_block(void)
{
    char doc[3072];
    char path[320];
    grbl_up = 1;

    printf("/status grbl block:\n");

    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "no file: no grbl block");

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\","
              "\"sender\":{\"connected\":true,\"generation\":3,\"for_s\":12,\"peer\":\"10.0.0.9\"},"
              "\"laser\":{\"armed\":false,\"arming\":false,\"model\":\"density\",\"floor_pct\":10},"
              "\"modals\":\"[GC:G0 G54]\",\"driver\":\"t\"}\n", now_mono());
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    if (!strstr(doc, "\"grbl\""))
        fprintf(stderr, "DOC: %s\n", doc);
    CHECK(strstr(doc, "\"grbl\":{\"age_s\":") != NULL, "fresh file: block present with age");
    CHECK(strstr(doc, "\"peer\":\"10.0.0.9\"") != NULL, "the report is embedded verbatim");
    CHECK(strstr(doc, "\"model\":\"density\"") != NULL, "the laser model rides the report");
    const char *p = strstr(doc, "\"age_s\":");
    CHECK(p && strtod(p + 8, NULL) < 5.0, "the age is computed from ts_mono");
    CHECK(doc[strlen(doc) - 1] == '}', "the document stays structurally intact");

    grbl_up = 0;
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "controller not running: no block, file or not");
    grbl_up = 1;

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\"", now_mono());   /* torn: no brace */
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "a torn body reads as absent");

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\",\"modals\":\"[GC:G0]\"}\n",
              now_mono() - 7200.0);
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "an absurd age reads as absent");
    snprintf(path, sizeof(path), "%s/grbl.state", dir);
    unlink(path);

    /* The lens block follows the crumb tray: the controller's marker in
     * the data directory moves the reach up by the offset on its grid
     * (1.35 in: 100 half-steps, 34.22 mm). */
    printf("/status lens block and the crumb tray:\n");
    char marker[320];
    snprintf(marker, sizeof(marker), "%s/tray.out", dir);
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"reach_min\":-0.07") && strstr(doc, "\"reach_max\":7.46") && strstr(doc, "\"tray\":\"in\"") &&
          strstr(doc, "\"tray_offset_mm\":34.29"), "tray in: the reach above the tray, and the offset");
    FILE *m = fopen(marker, "w");
    fputs("out\n", m);
    fclose(m);
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"reach_min\":34.15") && strstr(doc, "\"reach_max\":41.68") && strstr(doc, "\"tray\":\"out\""),
          "tray out: the reach moves up by the offset's grid");
    CHECK(strstr(doc, "\"edge_z\":3.35") != NULL, "tray out: the edge setting reads as set");
    unlink(marker);
    grbl_up = 0;
}

int main(void)
{
    snprintf(dir, sizeof(dir), "/tmp/forgectrl-status-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(root, sizeof(root), "%s/", dir);
    setenv("GF_SYSFS_ROOT", root, 1);       /* every machine attribute absent */
    setenv("GF_RUN_DIR", dir, 1);
    setenv("FORGECTRL_DATA_DIR", dir, 1);

    lid_gate();
    idle_fails_closed();
    sys_block();
    grbl_block();

    rmdir(dir);
    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the lid and idle reads fail closed, the sys block reports CPU and "
                      "memory, and the grbl block is echoed fresh and gated on a live controller\n",
           failures);
    return failures ? 1 : 0;
}
