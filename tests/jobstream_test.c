/*
 * jobstream_test.c - host test for the daemon's own Grbl sender
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A mock controller on a local port answers ok per line (error:9 for a
 * line that says BAD, and it records a soft reset), and a mock sysfs
 * tree carries the witnesses the test flips. Cases: a dark program
 * streams to its end; an error stops the run at the line that drew it;
 * a lit program ends after its dark stretch with the witnesses read; an
 * abort sends the soft reset; the generator's pacing return; the sender
 * check reads the state file.
 */
#include "../src/jobstream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char root[128];

static void sysfs_write(const char *attr, long v)
{
    char path[256], tmp[272];
    snprintf(path, sizeof(path), "%s/%s", root, attr);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%ld\n", v);
    fclose(f);
    rename(tmp, path);
}

/* ---- the mock controller ---- */

static struct {
    int listen_fd, port;
    int lines, resets, errors_sent;
    char seen[64][80];
    volatile int stop;
    int lit_after;              /* set hv after this many lines */
} mock;

static void *mock_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(mock.listen_fd, NULL, NULL);
        if (c < 0)
            break;
        const char *banner = "Grbl 1.1 ['$' for help]\r\n";
        if (write(c, banner, strlen(banner)) < 0) { }
        char buf[512];
        size_t n = 0;
        for (;;) {
            ssize_t r = read(c, buf + n, sizeof(buf) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
            buf[n] = '\0';
            char *nl;
            size_t i = 0;
            while (i < n) {
                if (buf[i] == 0x18) {
                    mock.resets++;
                    i++;
                    continue;
                }
                nl = memchr(buf + i, '\n', n - i);
                if (!nl)
                    break;
                *nl = '\0';
                if (mock.lines < 64)
                    snprintf(mock.seen[mock.lines], 80, "%s", buf + i);
                mock.lines++;
                const char *ans = strstr(buf + i, "BAD") ? "error:9\r\n" : "ok\r\n";
                if (strstr(buf + i, "BAD"))
                    mock.errors_sent++;
                if (mock.lit_after && mock.lines == mock.lit_after)
                    sysfs_write("pic/hv_current", 250);
                if (write(c, ans, strlen(ans)) < 0) { }
                i = (size_t)(nl + 1 - buf);
            }
            memmove(buf, buf + i, n - i);
            n -= i;
        }
        close(c);
        if (mock.stop)
            break;
    }
    return NULL;
}

static int mock_start(pthread_t *th)
{
    mock.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(mock.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(mock.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        return -1;
    socklen_t sl = sizeof(sa);
    getsockname(mock.listen_fd, (struct sockaddr *)&sa, &sl);
    mock.port = ntohs(sa.sin_port);
    listen(mock.listen_fd, 4);
    char port[16];
    snprintf(port, sizeof(port), "%d", mock.port);
    setenv("GF_GRBL_PORT", port, 1);
    return pthread_create(th, NULL, mock_thread, NULL);
}

/* The mock reads on its own thread: let a closed connection's last
 * bytes (the soft reset) land before the counts are read or cleared. */
static void mock_settle(void)
{
    struct timespec ts = { 0, 200 * 1000000L };
    nanosleep(&ts, NULL);
}

static void mock_reset_counts(void)
{
    mock_settle();
    mock.lines = mock.resets = mock.errors_sent = mock.lit_after = 0;
}

/* ---- the cases ---- */

static int samples_seen;
static void on_sample(void *ctx, const jobstream_sample_t *s)
{
    (void)ctx;
    (void)s;
    samples_seen++;
}

static void test_dark_program(void)
{
    mock_reset_counts();
    jobstream_lines_t lines = { "G21\nG90\n$100=53.4\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .sample = on_sample, .ctx = &lines,
                            .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    mock_settle();
    CHECK(rc == 0, "dark program failed: %s", err);
    CHECK(run.sent == 3 && run.acked == 3, "sent %d acked %d", run.sent, run.acked);
    CHECK(mock.lines == 3, "the mock saw %d lines", mock.lines);
    CHECK(!strcmp(mock.seen[2], "$100=53.4"), "line 3 was %s", mock.seen[2]);
    CHECK(mock.resets == 0, "a soft reset on a clean run");
    CHECK(samples_seen >= 1 && run.samples == samples_seen, "samples %d vs %d", samples_seen, run.samples);
    CHECK(!run.lit, "a dark run read as lit");
}

static void test_error_stops(void)
{
    mock_reset_counts();
    jobstream_lines_t lines = { "G21\nBAD LINE\nG90\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    mock_settle();
    CHECK(rc == -1, "the error did not stop the run");
    CHECK(strstr(err, "error:9") && strstr(err, "line 2") && strstr(err, "BAD LINE"),
          "the error names the wrong line: %s", err);
    CHECK(run.sent >= 2 && run.sent <= 3, "sent %d after the error", run.sent);
    CHECK(mock.resets == 1, "no soft reset after the error (%d)", mock.resets);
}

/* The tube's witnesses from another thread's view: the thermopile
 * and LASER_ON rise 0.4 s after the run starts, the tube goes dark
 * 0.4 s later. */
static void *flip(void *a)
{
    (void)a;
    struct timespec ts = { 0, 400 * 1000000L };
    nanosleep(&ts, NULL);
    sysfs_write("head/beam_detect_analog", 900);
    sysfs_write("cnc/laser_on_sampled", 40);
    nanosleep(&ts, NULL);
    sysfs_write("pic/hv_current", 0);
    return NULL;
}

static void test_lit_program(void)
{
    mock_reset_counts();
    sysfs_write("pic/hv_current", 0);
    sysfs_write("head/beam_detect_analog", 100);
    sysfs_write("cnc/laser_on_sampled", 0);
    mock.lit_after = 2;                 /* the tube lights as line 2 is taken */
    jobstream_lines_t lines = { "M3 S400\nG1 X10 F600\nM5\nM2\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines,
                            .wait_timeout_s = 10, .run_timeout_s = 20, .end_dark_s = 0.5 };
    jobstream_run_t run;
    char err[160] = "";
    pthread_t t;
    pthread_create(&t, NULL, flip, NULL);
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    pthread_join(t, NULL);
    mock_settle();
    CHECK(rc == 0, "lit program failed: %s", err);
    CHECK(run.lit, "the discharge was not seen");
    CHECK(run.hv_max == 250, "hv_max %ld", run.hv_max);
    CHECK(run.lon_max == 40, "lon_max %ld", run.lon_max);
    CHECK(run.tp_base == 100 && run.tp_max == 900, "thermopile base %ld max %ld", run.tp_base, run.tp_max);
    CHECK(run.t_last_lit >= run.t_first_lit, "lit times");
    CHECK(mock.resets == 0, "a soft reset on a clean lit run");
}

static void test_dark_when_lit_expected(void)
{
    mock_reset_counts();
    sysfs_write("pic/hv_current", 0);
    jobstream_lines_t lines = { "M3 S400\nM5\nM2\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines,
                            .wait_timeout_s = 10, .run_timeout_s = 20 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    mock_settle();
    CHECK(rc == -1 && strstr(err, "without a discharge"), "a dark run passed as lit: rc %d %s", rc, err);
    CHECK(mock.resets == 1, "no soft reset (%d)", mock.resets);
}

static volatile int abort_flag;
static int slow_gen(void *ctx, char *buf, size_t len)
{
    int *n = ctx;
    if (*n == 0) {
        (*n)++;
        snprintf(buf, len, "G4 P0.1");
        return 1;
    }
    abort_flag = 1;
    return 2;                       /* pacing: nothing yet */
}

static void test_abort_and_pacing(void)
{
    mock_reset_counts();
    int n = 0;
    jobstream_cfg_t cfg = { .gen = slow_gen, .ctx = &n, .abort_flag = &abort_flag, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    abort_flag = 0;
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    mock_settle();
    CHECK(rc == -1 && !strcmp(err, "aborted"), "abort: rc %d %s", rc, err);
    CHECK(run.sent == 1, "sent %d", run.sent);
    CHECK(mock.resets == 1, "no soft reset on abort (%d)", mock.resets);
}

static void test_sender_check(void)
{
    char dir[160], path[256];
    snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0700);
    setenv("GF_RUN_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/grbl.state", dir);
    unlink(path);
    CHECK(jobstream_sender_connected() == -1, "no file reads as known");
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"sender\":{\"connected\":true,\"generation\":3}}\n");
    fclose(f);
    CHECK(jobstream_sender_connected() == 1, "connected not read");
    f = fopen(path, "w");
    fprintf(f, "{\"sender\":{\"connected\":false}}\n");
    fclose(f);
    CHECK(jobstream_sender_connected() == 0, "disconnected not read");
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/jobstream_test.%d", (int)getpid());
    char sub[256];
    mkdir(root, 0700);
    snprintf(sub, sizeof(sub), "%s/pic", root);
    mkdir(sub, 0700);
    snprintf(sub, sizeof(sub), "%s/head", root);
    mkdir(sub, 0700);
    snprintf(sub, sizeof(sub), "%s/cnc", root);
    mkdir(sub, 0700);
    setenv("GF_SYSFS_ROOT", root, 1);
    setenv("FFLOG_STDERR", "1", 1);
    sysfs_write("pic/hv_current", 0);
    sysfs_write("head/beam_detect_analog", 100);
    sysfs_write("cnc/laser_on_sampled", 0);
    for (int i = 1; i <= 4; i++) {
        char a[32];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        sysfs_write(a, 100);
    }
    pthread_t th;
    if (mock_start(&th) != 0) {
        printf("cannot start the mock controller\n");
        return 1;
    }
    test_dark_program();
    test_error_stops();
    test_lit_program();
    test_dark_when_lit_expected();
    test_abort_and_pacing();
    test_sender_check();
    mock.stop = 1;
    shutdown(mock.listen_fd, SHUT_RDWR);
    close(mock.listen_fd);
    pthread_join(th, NULL);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    if (fails) {
        printf("jobstream_test: %d failures\n", fails);
        return 1;
    }
    printf("jobstream_test: ok\n");
    return 0;
}
