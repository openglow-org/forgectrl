/*
 * jobrun_test.c - host test for the job runner
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The mock controller of jobstream_test, with one addition: it holds the
 * ok of a line that says P77 until the test lets go, so a run can be
 * looked at while it plays. Cases: a posted program's check (what is
 * stripped, every offense with its line number); a program plays as
 * job:<name> with the machine lease held as a sender, refuses a second
 * program and a wizard in the holder's words, gets the runner's M2, and
 * leaves the lease free and its staging file gone; a program that ends
 * in M2 gets no second one; the runner's $X goes out first when asked
 * for, and never otherwise; a program is done when the kernel is idle,
 * not at its last ok; the abort sends the soft reset and frees the
 * lease; a connected Grbl client refuses the start with nothing sent; a
 * run on the caller's thread stands inside a wizard's hold and gives the
 * machine back to the wizard, and is not the abort route's to stop; the
 * ready and done hooks of a started run, with the lease given back from
 * inside done; the witness rate of a program that must light and of one
 * that need not; a program that must light and does not, fails.
 */
#include "../src/jobrun.h"
#include "../src/lease.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char root[128], staged[192];

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- the mock controller ---- */

static struct {
    int listen_fd;
    volatile int lines, resets, holding, release;
    char seen[64][80];
} mock;

static void *mock_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(mock.listen_fd, NULL, NULL);
        if (c < 0)
            break;
        char buf[1024];
        size_t n = 0;
        for (;;) {
            ssize_t r = read(c, buf + n, sizeof(buf) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
            size_t i = 0;
            while (i < n) {
                if (buf[i] == 0x18) {
                    mock.resets++;
                    i++;
                    continue;
                }
                char *nl = memchr(buf + i, '\n', n - i);
                if (!nl)
                    break;
                *nl = '\0';
                if (mock.lines < 64)
                    snprintf(mock.seen[mock.lines], 80, "%s", buf + i);
                mock.lines++;
                if (strstr(buf + i, "P77")) {
                    mock.holding = 1;
                    /* Let go by the test, or by an abort: its soft reset
                     * behind whatever lines were in flight, or the close. */
                    while (!mock.release) {
                        char ahead[512];
                        ssize_t k = recv(c, ahead, sizeof(ahead), MSG_DONTWAIT | MSG_PEEK);
                        if (k == 0 || (k > 0 && memchr(ahead, 0x18, (size_t)k)))
                            break;
                        msleep(10);
                    }
                    mock.holding = 0;
                }
                if (write(c, "ok\r\n", 4) < 0) { }
                i = (size_t)(nl + 1 - buf);
            }
            memmove(buf, buf + i, n - i);
            n -= i;
        }
        close(c);
    }
    return NULL;
}

static void mock_start(void)
{
    pthread_t th;
    mock.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(mock.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(mock.listen_fd, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t sl = sizeof(sa);
    getsockname(mock.listen_fd, (struct sockaddr *)&sa, &sl);
    listen(mock.listen_fd, 4);
    char port[16];
    snprintf(port, sizeof(port), "%d", ntohs(sa.sin_port));
    setenv("GF_GRBL_PORT", port, 1);
    pthread_create(&th, NULL, mock_thread, NULL);
    pthread_detach(th);
}

static void mock_clear(void)
{
    msleep(200);                        /* a closed connection's last bytes land first */
    mock.lines = mock.resets = mock.holding = mock.release = 0;
}

static int wait_for(volatile int *flag, int want, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        if (*flag == want)
            return 1;
        msleep(10);
    }
    return *flag == want;
}

/* ---- helpers ---- */

static void stage(const char *text, size_t len)
{
    FILE *f = fopen(staged, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}
#define STAGE(s) stage(s, sizeof(s) - 1)

static const char *status(void)
{
    static char body[768];
    if (jobrun_status_json(body, sizeof(body)) != 0)
        snprintf(body, sizeof(body), "(no record)");
    return body;
}

static int wait_state(const char *want, int ms)
{
    char key[32];
    snprintf(key, sizeof(key), "\"state\":\"%s\"", want);
    for (int t = 0; t < ms; t += 20) {
        if (strstr(status(), key))
            return 1;
        msleep(20);
    }
    return 0;
}

static int client_on;
static int client_connected(void)
{
    return client_on;
}

/* ---- the cases ---- */

static void offense(const char *text, size_t len, const char *line, const char *words)
{
    char err[160] = "";
    int n = -1;
    stage(text, len);
    int rc = jobrun_program_check(staged, &n, err, sizeof(err));
    CHECK(rc == -1 && strstr(err, line) && strstr(err, words), "wanted \"%s ... %s\", got rc %d \"%s\"",
          line, words, rc, err);
}
#define OFFENSE(s, line, words) offense(s, sizeof(s) - 1, line, words)

static void test_check(void)
{
    char err[160] = "";
    int n = -1;
    STAGE("%\r\n(a header! with ~ and ?)\r\nG21 ; metric?\r\n\r\n  G90\t\r\nG1 X10 (in) F600\n%\n");
    CHECK(jobrun_program_check(staged, &n, err, sizeof(err)) == 0 && n == 3, "clean program: n %d \"%s\"", n, err);

    OFFENSE("G21\n$X\nG90\n", "line 2", "$ command");
    OFFENSE("G21\n  $H\n", "line 2", "$ command");
    OFFENSE("G1 X1\nG1 X2!\n", "line 2", "realtime");
    OFFENSE("G1 X1 ?\n", "line 1", "realtime");
    OFFENSE("G21\nG1 X\x9e" "1\n", "line 2", "printable");
    OFFENSE("G21\n\x18\n", "line 2", "printable");
    OFFENSE("G21\nG1\0X1\n", "line 2", "printable");
    OFFENSE("G21\nG1 X1\rG1 X2\n", "line 2", "carriage return");
    OFFENSE("(only a comment)\n\n%\n", "the program", "no lines");

    char longline[400];
    memset(longline, 'X', sizeof(longline));
    memcpy(longline, "G1 ", 3);
    longline[sizeof(longline) - 1] = '\n';
    offense(longline, sizeof(longline), "line 1", "longer");
    /* A long comment is no offense: it never goes out. */
    memcpy(longline, "G1(", 3);
    longline[sizeof(longline) - 2] = ')';
    stage(longline, sizeof(longline));
    CHECK(jobrun_program_check(staged, &n, err, sizeof(err)) == 0 && n == 1, "a long comment: n %d \"%s\"", n, err);

    CHECK(jobrun_name_ok("panel") == 0 && jobrun_name_ok("a.b_c-9") == 0 &&
          jobrun_name_ok("org.example.a-package-id-of-sixty-three-characters-at-the-bound") == 0,
          "good names refused (a package id of 63 characters is one)");
    CHECK(jobrun_name_ok("") != 0 && jobrun_name_ok("a b") != 0 && jobrun_name_ok("a\"b") != 0 &&
          jobrun_name_ok("0123456789012345678901234567890123456789012345678901234567890123") != 0,
          "a bad name passed");
}

static void test_program_plays(void)
{
    char err[160] = "", who[64] = "", lease[512];
    mock_clear();
    STAGE("(header)\nG21\nG4 P77 ; held\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "test", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(access(staged, F_OK) != 0, "the staged file kept its name under a job that plays");
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");

    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "job:test"), "the holder is \"%s\"", who);
    lease_json(lease, sizeof(lease));
    CHECK(strstr(lease, "\"kind\":\"sender\"") && strstr(lease, "a job (test)"), "lease: %s", lease);
    const char *st = status();
    CHECK(strstr(st, "\"state\":\"running\"") && strstr(st, "\"owner\":\"job:test\"") &&
          strstr(st, "\"program\":true") && strstr(st, "\"lines\":3"), "record while it plays: %s", st);

    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "second", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "a job (test) holds the machine"), "a second program: \"%s\"", err);
    CHECK(access(staged, F_OK) != 0, "a refused program was left staged");
    CHECK(lease_take("wizard:motion", LEASE_HARDWARE, NULL, err, sizeof(err)) == -1 &&
          strstr(err, "a job (test) holds the machine"), "a wizard beside the job: \"%s\"", err);

    mock.release = 1;
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
    msleep(200);
    CHECK(mock.lines == 4, "the controller saw %d lines", mock.lines);
    CHECK(!strcmp(mock.seen[0], "G21") && !strcmp(mock.seen[1], "G4 P77") &&
          !strcmp(mock.seen[2], "G1 X5 F600") && !strcmp(mock.seen[3], "M2"),
          "the lines out: \"%s\" \"%s\" \"%s\" \"%s\"", mock.seen[0], mock.seen[1], mock.seen[2], mock.seen[3]);
    CHECK(mock.resets == 0, "a soft reset on a clean run");
    st = status();
    CHECK(strstr(st, "\"sent\":4") && strstr(st, "\"acked\":4") && strstr(st, "\"lit\":false") &&
          strstr(st, "\"reason\":\"\""), "record at the end: %s", st);
}

static void test_own_m2(void)
{
    char err[160] = "";
    mock_clear();
    STAGE("G21\nM5\nM2\n");
    CHECK(jobrun_program_start(staged, "ends", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    msleep(200);
    CHECK(mock.lines == 3 && !strcmp(mock.seen[2], "M2"), "%d lines, the last \"%s\"", mock.lines, mock.seen[2]);
}

static void test_unlock(void)
{
    char err[160] = "";
    mock_clear();
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "unlock", NULL, 0, 30, 1, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    msleep(200);
    CHECK(mock.lines == 3 && !strcmp(mock.seen[0], "$X") && !strcmp(mock.seen[1], "G21") &&
          !strcmp(mock.seen[2], "M2"), "%d lines: \"%s\" \"%s\" \"%s\"", mock.lines, mock.seen[0],
          mock.seen[1], mock.seen[2]);
}

static void kernel_state(const char *word)
{
    char path[192], tmp[200];
    snprintf(path, sizeof(path), "%s/cnc/state", root);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    fprintf(f, "%s\n", word);
    fclose(f);
    rename(tmp, path);
}

/* Every line is acknowledged while the kernel still plays the last of
 * the ring: the record must not say done until it is idle. */
static void test_done_is_idle(void)
{
    char err[160] = "", who[64];
    mock_clear();
    kernel_state("running");
    STAGE("G21\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "tail", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    msleep(1000);
    CHECK(strstr(status(), "\"acked\":3") && strstr(status(), "\"state\":\"running\""),
          "with the kernel still playing: %s", status());
    CHECK(lease_holder(who, sizeof(who)), "the lease went back while the kernel played");
    kernel_state("idle");
    CHECK(wait_state("done", 2000), "the job did not end once the kernel was idle: %s", status());
}

static void test_abort(void)
{
    char err[160] = "", who[64];
    mock_clear();
    CHECK(jobrun_program_abort(err, sizeof(err)) == -1 && strstr(err, "no job"), "abort of nothing: \"%s\"", err);
    STAGE("G21\nG4 P77\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "stopme", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    CHECK(jobrun_program_abort(err, sizeof(err)) == 0, "abort: \"%s\"", err);
    /* The abort waits for the run: the record and the lease are final here. */
    const char *st = status();
    CHECK(strstr(st, "\"state\":\"failed\"") && strstr(st, "aborted"), "record after the abort: %s", st);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
    /* The three lines were in flight behind the held one; the runner's
     * M2 is a barrier and never went out. */
    CHECK(mock.lines == 3 && strcmp(mock.seen[2], "M2"), "%d lines went out, the last \"%s\"",
          mock.lines, mock.seen[2]);
}

static void test_client_refuses(void)
{
    char err[160] = "", who[64];
    mock_clear();
    client_on = 1;
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "test", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "a sender is connected"), "with a client on the socket: \"%s\"", err);
    client_on = 0;
    msleep(200);
    CHECK(mock.lines == 0, "%d lines went out past the refusal", mock.lines);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is \"%s\" after a refusal", who);
    CHECK(access(staged, F_OK) != 0, "a refused program was left staged");
    /* A refused start is nobody's job: the record is still the last one's. */
    CHECK(strstr(status(), "\"owner\":\"job:stopme\"") && strstr(status(), "aborted"),
          "the record after a refusal: %s", status());
}

static void *sync_thread(void *arg)
{
    int *rc = arg;
    static jobstream_lines_t lines;
    lines = (jobstream_lines_t){ "G21\nG4 P77\nM2\n", 0 };
    jobrun_cfg_t cfg = { .owner = "job:laser.focus", .under = "wizard:laser.focus",
                         .stream = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 30 } };
    jobstream_run_t run;
    char err[160] = "";
    *rc = jobrun_sync(&cfg, &run, err, sizeof(err));
    if (*rc != 0)
        printf("sync: %s\n", err);
    return NULL;
}

static void test_sync_under_a_wizard(void)
{
    char err[160] = "", who[64] = "", lease[512];
    mock_clear();
    CHECK(lease_take("wizard:laser.focus", LEASE_HARDWARE, NULL, err, sizeof(err)) == 0, "the wizard: %s", err);

    /* Not under the wizard: refused in the wizard's words. */
    jobstream_lines_t lines = { "G21\n", 0 };
    jobrun_cfg_t bare = { .owner = "job:x", .stream = { .gen = jobstream_lines_gen, .ctx = &lines } };
    jobstream_run_t run;
    CHECK(jobrun_sync(&bare, &run, err, sizeof(err)) == -1 && strstr(err, "a setup wizard (laser.focus) holds"),
          "a bare job beside a wizard: \"%s\"", err);

    int rc = -2;
    pthread_t t;
    pthread_create(&t, NULL, sync_thread, &rc);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    lease_json(lease, sizeof(lease));
    CHECK(strstr(lease, "\"owner\":\"job:laser.focus\"") && strstr(lease, "\"under\":\"wizard:laser.focus\""),
          "lease inside the wizard: %s", lease);
    CHECK(strstr(status(), "\"program\":false"), "record: %s", status());
    CHECK(jobrun_program_abort(err, sizeof(err)) == -1 && strstr(err, "stop it where it was started"),
          "the abort route on a wizard's job: \"%s\"", err);
    mock.release = 1;
    pthread_join(t, NULL);
    CHECK(rc == 0, "the wizard's job: rc %d", rc);
    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "wizard:laser.focus"), "back to \"%s\"", who);
    lease_release("wizard:laser.focus");
}

static int ready_fail, done_calls, done_rc, held_in_done, held_after_back;
static int on_ready(void *ctx, char *err, size_t elen)
{
    (void)ctx;
    if (ready_fail)
        snprintf(err, elen, "the keys are held");
    return ready_fail ? -1 : 0;
}
static void on_done(void *ctx, int rc, const jobstream_run_t *run, const char *err)
{
    char who[64];
    (void)ctx;
    (void)run;
    (void)err;
    done_calls++;
    done_rc = rc;
    held_in_done = lease_holder(who, sizeof(who)) && !strcmp(who, "recorder");
    jobrun_lease_back();
    held_after_back = lease_holder(who, sizeof(who));
}

static void test_hooks(void)
{
    char err[160] = "", who[64];
    static jobstream_lines_t lines;
    mock_clear();
    lines = (jobstream_lines_t){ "G21\nM2\n", 0 };
    jobrun_cfg_t cfg = { .owner = "recorder", .ready = on_ready, .done = on_done,
                         .stream = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 30 } };
    ready_fail = 1;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == -1 && strstr(err, "the keys are held"), "ready's refusal: \"%s\"", err);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is \"%s\" after ready refused", who);
    CHECK(!strstr(status(), "\"owner\":\"recorder\""), "ready's refusal rewrote the record: %s", status());
    msleep(200);
    CHECK(mock.lines == 0 && done_calls == 0, "%d lines, %d done calls past the refusal", mock.lines, done_calls);

    ready_fail = 0;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the run did not end: %s", status());
    CHECK(done_calls == 1 && done_rc == 0, "done: %d calls, rc %d", done_calls, done_rc);
    CHECK(held_in_done, "done ran without the lease");
    CHECK(!held_after_back, "the lease was still held after jobrun_lease_back()");
    jobrun_stop("recorder");            /* nothing to abort: only the join */

    /* The owner's stop aborts its own run and nobody else's. */
    mock_clear();
    lines = (jobstream_lines_t){ "G4 P77\nM2\n", 0 };
    done_calls = 0;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    jobrun_stop("job:somebody");
    CHECK(strstr(status(), "\"state\":\"running\""), "somebody else's stop ended it: %s", status());
    jobrun_stop("recorder");
    CHECK(done_calls == 1 && done_rc == -1, "done after the stop: %d calls, rc %d", done_calls, done_rc);
    CHECK(strstr(status(), "aborted"), "record: %s", status());
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
}

static int samples_now(void)
{
    const char *k = strstr(status(), "\"samples\":");
    return k ? atoi(k + 10) : -1;
}

/* A program that must light is witnessed at the full rate, any other at
 * a fifth of it: a second into each, held at the same line. */
static void test_witness_rate(void)
{
    char err[160] = "";
    for (int must_light = 0; must_light < 2; must_light++) {
        mock_clear();
        STAGE("G21\nG4 P77\n");
        CHECK(jobrun_program_start(staged, "rate", NULL, must_light ? 30 : 0, 30, 0, err, sizeof(err)) == 0,
              "start: %s", err);
        CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
        msleep(1000);
        int n = samples_now();
        if (must_light)
            CHECK(n >= 18, "a program that must light had %d samples in its first second", n);
        else
            CHECK(n >= 3 && n <= 9, "a program with no such claim had %d samples in its first second", n);
        jobrun_program_abort(err, sizeof(err));
    }
}

static void test_must_light(void)
{
    char err[160] = "";
    mock_clear();
    STAGE("M3 S100\nG1 X1 F600\nM5\n");
    CHECK(jobrun_program_start(staged, "dark", NULL, 5, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 8000), "a dark run passed as lit: %s", status());
    CHECK(strstr(status(), "without a discharge"), "record: %s", status());
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/jobrun_test.%d", (int)getpid());
    mkdir(root, 0755);
    char sub[192];
    const char *dirs[] = { "pic", "head", "cnc" };
    for (int i = 0; i < 3; i++) {
        snprintf(sub, sizeof(sub), "%s/%s", root, dirs[i]);
        mkdir(sub, 0755);
    }
    setenv("GF_SYSFS_ROOT", root, 1);
    snprintf(staged, sizeof(staged), "%s/job.upload", root);
    lease_sender_connected = client_connected;
    mock_start();

    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "check", test_check }, { "program-plays", test_program_plays }, { "own-m2", test_own_m2 }, { "unlock", test_unlock }, { "done-is-idle", test_done_is_idle },
        { "abort", test_abort }, { "client-refuses", test_client_refuses },
        { "sync-under-a-wizard", test_sync_under_a_wizard }, { "hooks", test_hooks },
        { "witness-rate", test_witness_rate }, { "must-light", test_must_light },
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);           /* the mock answers a connection an abort has closed */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("case %s\n", cases[i].name);
        cases[i].run();
    }

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "jobrun_test: %d FAILED\n" : "jobrun_test: all passed\n", fails);
    return fails ? 1 : 0;
}
