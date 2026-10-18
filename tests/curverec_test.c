/*
 * curverec_test.c - host test for the dose-curve recorder's run
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A mock controller on a local port answers ok per line and holds the ok
 * of the ladder's M3 until the soft reset comes, so the run waits as it
 * does for a press that never comes. The settings are the test's own
 * store; the sysfs tree is empty, so every witness reads dark. The
 * override keeps its marker under /data: the test is linked with fopen
 * and unlink wrapped, and that one path lands in the test's tree.
 *
 * The recorder: its start makes it the lease holder, as a sender, and
 * the job record's owner, not a posted program; the floor is 0 and the
 * curve off while it runs; the abort route refuses its run (stop it
 * where it was started), and a posted job is refused in its name; its
 * stop ends the run failed, stopped before the ladder fired, with the
 * lease free; and the dose keys read back as they were before the start.
 */
#define _GNU_SOURCE
#include "../src/curverec.h"
#include "../src/jobrun.h"
#include "../src/lease.h"
#include "../src/settings.h"

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

static char root[128], marker[192], staged[192];

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- the settings store ---- */

/* settings.c's contract over a table: -1 for a key that is absent, and
 * an empty value removes the key. */
#define STORE_MAX 8
static struct { char key[40], val[192]; } store[STORE_MAX];
static int nstore;

int settings_get(const char *key, char *val, size_t len)
{
    for (int i = 0; i < nstore; i++)
        if (!strcmp(store[i].key, key)) {
            snprintf(val, len, "%s", store[i].val);
            return 0;
        }
    return -1;
}

int settings_set(const char *key, const char *val)
{
    int i = 0;
    while (i < nstore && strcmp(store[i].key, key))
        i++;
    if (!*val) {
        if (i < nstore)
            store[i] = store[--nstore];
        return 0;
    }
    if (i == nstore) {
        if (nstore == STORE_MAX)
            return -1;
        snprintf(store[nstore++].key, sizeof(store[0].key), "%s", key);
    }
    snprintf(store[i].val, sizeof(store[i].val), "%s", val);
    return 0;
}

static const char *key(const char *k)
{
    static char v[192];
    if (settings_get(k, v, sizeof(v)) != 0)
        snprintf(v, sizeof(v), "(unset)");
    return v;
}

/* ---- the override's marker, in the test's tree ---- */

FILE *__real_fopen(const char *path, const char *mode);
int __real_unlink(const char *path);

static const char *redirect(const char *path)
{
    return !strcmp(path, "/data/forgefirm/curverec.saved") ? marker : path;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    return __real_fopen(redirect(path), mode);
}

int __wrap_unlink(const char *path)
{
    return __real_unlink(redirect(path));
}

/* ---- the mock controller ---- */

static struct {
    int listen_fd;
    volatile int holding;
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
                    i++;
                    continue;
                }
                char *nl = memchr(buf + i, '\n', n - i);
                if (!nl)
                    break;
                *nl = '\0';
                if (!strcmp(buf + i, "M3")) {
                    mock.holding = 1;
                    /* Let go by the soft reset behind whatever lines were
                     * in flight, or by the close. */
                    for (;;) {
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

static const char *job(void)
{
    static char body[768];
    if (jobrun_status_json(body, sizeof(body)) != 0)
        snprintf(body, sizeof(body), "(no record)");
    return body;
}

static const char *recorder(void)
{
    static char body[1024];
    curverec_status_json(body, sizeof(body));
    return body;
}

#define FLOOR_BEFORE "12"
#define CURVE_BEFORE "10:3.1,50:41.7,100:100"

/* ---- the cases, in the order of one recording ---- */

static void test_start(void)
{
    char err[160] = "", who[64] = "", lease[512];
    settings_set("laser_floor_density", FLOOR_BEFORE);
    settings_set("laser_dose_curve", CURVE_BEFORE);
    CHECK(curverec_start(err, sizeof(err)) == 0, "start: %s", err);
    CHECK(strstr(recorder(), "\"state\":\"waiting\""), "the recorder after its start: %s", recorder());
    CHECK(wait_for(&mock.holding, 1, 3000), "the ladder's M3 never arrived");

    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "recorder"), "the holder is \"%s\"", who);
    lease_json(lease, sizeof(lease));
    CHECK(strstr(lease, "\"kind\":\"sender\""), "lease: %s", lease);
    const char *st = job();
    CHECK(strstr(st, "\"state\":\"running\"") && strstr(st, "\"owner\":\"recorder\"") &&
          strstr(st, "\"program\":false"), "the job record while the recorder runs: %s", st);
}

static void test_override(void)
{
    CHECK(!strcmp(key("laser_floor_density"), "0"), "the floor while it runs: %s", key("laser_floor_density"));
    CHECK(!strcmp(key("laser_dose_curve"), "off"), "the curve while it runs: %s", key("laser_dose_curve"));
}

static void test_abort_route(void)
{
    char err[160] = "";
    CHECK(jobrun_program_abort(err, sizeof(err)) == -1 && strstr(err, "stop it where it was started"),
          "the abort route on the recorder's run: \"%s\"", err);
}

static void test_posted_job(void)
{
    char err[160] = "", who[64] = "";
    FILE *f = fopen(staged, "w");
    fputs("G21\n", f);
    fclose(f);
    CHECK(jobrun_program_start(staged, "beside", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "the dose-curve recorder holds the machine"), "a posted job beside the recorder: \"%s\"", err);
    /* Neither refusal touched the run: it still waits at its M3. */
    msleep(200);
    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "recorder") && mock.holding &&
          strstr(job(), "\"owner\":\"recorder\""), "after the refusals: holder \"%s\", %s", who, job());
}

static void test_stop(void)
{
    char who[64] = "";
    curverec_stop();
    /* The stop waits for the run: the record and the lease are final here. */
    const char *r = recorder();
    CHECK(strstr(r, "\"state\":\"failed\"") && strstr(r, "stopped before the ladder fired"),
          "the recorder after the stop: %s", r);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
}

static void test_keys_restored(void)
{
    CHECK(!strcmp(key("laser_floor_density"), FLOOR_BEFORE), "the floor after the stop: %s",
          key("laser_floor_density"));
    CHECK(!strcmp(key("laser_dose_curve"), CURVE_BEFORE), "the curve after the stop: %s",
          key("laser_dose_curve"));
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/curverec_test.%d", (int)getpid());
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
    mkdir(root, 0755);
    setenv("GF_SYSFS_ROOT", root, 1);
    snprintf(marker, sizeof(marker), "%s/curverec.saved", root);
    snprintf(staged, sizeof(staged), "%s/job.upload", root);
    mock_start();

    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "start", test_start }, { "override", test_override }, { "abort-route", test_abort_route },
        { "posted-job", test_posted_job }, { "stop", test_stop }, { "keys-restored", test_keys_restored },
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);           /* the mock answers a connection the stop has closed */
    alarm(60);                          /* a stop that never returns fails the test, in its case */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("case %s\n", cases[i].name);
        cases[i].run();
    }

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "curverec_test: %d FAILED\n" : "curverec_test: all passed\n", fails);
    return fails ? 1 : 0;
}
