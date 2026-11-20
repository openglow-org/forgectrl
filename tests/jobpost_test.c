/*
 * jobpost_test.c - host test for POST /job's multipart reader
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The route and its file sink on a ulfius instance of their own on
 * 127.0.0.1, wired as main.c wires them, fed raw multipart bodies over a
 * socket. The guards are stubs that let every request in; the program's
 * check and the job's name are the runner's own (jobrun.c); the start is
 * wrapped (--wrap=jobrun_program_start), so a program that passes is
 * recorded as the runner would be handed it, and nothing plays. The
 * staging file lives under GF_RUN_DIR, a temporary directory.
 *
 *   the form parts: a program of several kilobytes (many chunks of the
 *     sink) reaches the start byte for byte, with the name and unlock=1;
 *     without the unlock field the start is told 0
 *   a program that names no timeout_s gets the longest a client may
 *     name (86400 s); one that names a bound keeps it
 *   a second upload begun while the first is handed to the runner is
 *     refused, however long since the first's last chunk: the first runs
 *     its own lines
 *   a missing name: 400, nothing started, nothing staged
 *   a $ line: 400 naming its line number, nothing started, nothing staged
 *   a malformed boundary (the body's delimiter is not the one declared):
 *     400, nothing started, nothing staged
 *   an abandoned upload is adopted by no request on another thread, even
 *     one whose structure sits at the abandoned request's address: 400
 */
#include "../src/auth.h"
#include "../src/jobpost.h"
#include "../src/jobrun.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <ulfius.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define PORT 18471
#define BOUNDARY "jobpostTestBoundary7f3"

static char rundir[128], staged[192];

/* ---- the guards: every request is let in ---- */

int auth_read_ok(const struct _u_request *req, struct _u_response *res)
{
    (void)req; (void)res;
    return 1;
}

int auth_write_ok(const struct _u_request *req, struct _u_response *res)
{
    (void)req; (void)res;
    return 1;
}

int auth_write_permitted_cap(const struct _u_request *req, const char *cap)
{
    (void)req; (void)cap;
    return 1;
}

int auth_by_host(void)
{
    return 0;
}

/* ---- the start, as the runner is handed it ---- */

static struct {
    int calls;
    char name[64];
    int unlock;
    int under;
    double run_timeout_s;
    char *program;
    size_t len;
} started;

/* Between the route's look at the upload and the runner's start, a second
 * upload begins (another client's first chunk). */
static int second_upload;
static struct _u_request second_req;

/* The monotonic clock, moved on by the test (the link wraps clock_gettime). */
static double clock_skew_s;
int __real_clock_gettime(clockid_t id, struct timespec *ts);
int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    int rc = __real_clock_gettime(id, ts);
    if (rc == 0 && id == CLOCK_MONOTONIC && clock_skew_s > 0) {
        ts->tv_sec += (time_t)clock_skew_s;
    }
    return rc;
}

int __wrap_jobrun_program_start(const char *path, const char *name, const char *under,
                                double lit_timeout_s, double run_timeout_s, int unlock,
                                char *err, size_t elen)
{
    (void)lit_timeout_s; (void)err; (void)elen;
    if (second_upload) {
        /* Past the minute an abandoned upload is kept: the start alone
         * holds the slot. */
        second_upload = 0;
        clock_skew_s += 61;
        jobpost_sink(&second_req, "program", "G1 X150 Y30 F1200 ; B's\n", 0, 24);
    }
    started.calls++;
    started.run_timeout_s = run_timeout_s;
    snprintf(started.name, sizeof(started.name), "%s", name ? name : "");
    started.unlock = unlock;
    started.under = under != NULL;
    free(started.program);
    started.program = NULL;
    started.len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        started.program = malloc((size_t)n + 1);
        if (started.program && fread(started.program, 1, (size_t)n, f) == (size_t)n)
            started.len = (size_t)n;
        fclose(f);
    }
    unlink(path);                       /* the runner takes the name away */
    return 0;
}

/* ---- the server, wired as main.c wires it ---- */

static int sink(const struct _u_request *req, const char *key, const char *filename,
                const char *content_type, const char *transfer_encoding,
                const char *data, uint64_t off, size_t size, void *cls)
{
    (void)filename; (void)content_type; (void)transfer_encoding; (void)cls;
    jobpost_sink(req, key, data, off, size);
    return U_OK;
}

/* A POST /job with no file part, on a thread of its own, made with the
 * request structure it is handed. */
static int stranger_status;
static void *stranger_post(void *arg)
{
    struct _u_request *r = arg;
    struct _u_response res;
    ulfius_init_response(&res);
    u_map_put(r->map_post_body, "name", "stranger");
    cb_job_post(r, &res, NULL);
    stranger_status = (int)res.status;
    ulfius_clean_response(&res);
    return NULL;
}

/* ---- the client ---- */

/* POST /job with this content type and body: the status, and the body
 * of the answer in out. */
static int post(const char *ctype, const char *body, size_t blen, char *out, size_t olen)
{
    out[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    char head[256];
    int hn = snprintf(head, sizeof(head),
                      "POST /job HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: %s\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", ctype, blen);
    if (write(fd, head, (size_t)hn) != hn || write(fd, body, blen) != (ssize_t)blen) {
        close(fd);
        return -1;
    }
    static char resp[8192];
    size_t got = 0;
    ssize_t n;
    while (got + 1 < sizeof(resp) && (n = read(fd, resp + got, sizeof(resp) - 1 - got)) > 0)
        got += (size_t)n;
    resp[got] = '\0';
    close(fd);
    int status = -1;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
        return -1;
    const char *b = strstr(resp, "\r\n\r\n");
    if (b)
        snprintf(out, olen, "%s", b + 4);
    return status;
}

/* A multipart form: the fields (name=value pairs, NULL-ended), then the
 * program as the file part. The caller frees it. */
static char *form(const char *boundary, const char *const *fields, const char *program, size_t *len)
{
    size_t cap = strlen(program) + 4096;
    char *b = malloc(cap);
    size_t o = 0;
    for (; fields && *fields; fields += 2)
        o += (size_t)snprintf(b + o, cap - o, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                              boundary, fields[0], fields[1]);
    o += (size_t)snprintf(b + o, cap - o, "--%s\r\nContent-Disposition: form-data; name=\"program\"; "
                          "filename=\"job.gcode\"\r\nContent-Type: application/octet-stream\r\n\r\n", boundary);
    memcpy(b + o, program, strlen(program));
    o += strlen(program);
    o += (size_t)snprintf(b + o, cap - o, "\r\n--%s--\r\n", boundary);
    *len = o;
    return b;
}

static int send_form(const char *const *fields, const char *program, char *out, size_t olen)
{
    size_t len;
    char *b = form(BOUNDARY, fields, program, &len);
    int st = post("multipart/form-data; boundary=" BOUNDARY, b, len, out, olen);
    free(b);
    return st;
}

static int is_staged(void)
{
    struct stat sb;
    return stat(staged, &sb) == 0;
}

int main(void)
{
    char out[1024];
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
    snprintf(rundir, sizeof(rundir), "/tmp/jobpost_test.XXXXXX");
    if (!mkdtemp(rundir)) {
        perror("mkdtemp");
        return 2;
    }
    setenv("GF_RUN_DIR", rundir, 1);
    setenv("FFLOG_SOCK", "/nonexistent/jobpost_test", 1);
    snprintf(staged, sizeof(staged), "%s/job.upload", rundir);

    struct _u_instance inst;
    struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ulfius_init_instance(&inst, PORT, &bind_addr, NULL) != U_OK) {
        printf("cannot init ulfius\n");
        return 2;
    }
    inst.max_post_body_size = 64 * 1024;
    ulfius_init_request(&second_req);
    second_req.http_url = strdup("/job");       /* ulfius_init_request leaves it NULL */
    ulfius_add_endpoint_by_val(&inst, "POST", NULL, "/job", 0, cb_job_post, NULL);
    ulfius_set_upload_file_callback_function(&inst, &sink, NULL);
    if (ulfius_start_framework(&inst) != U_OK) {
        printf("cannot listen on 127.0.0.1:%d\n", PORT);
        return 2;
    }

    /* The form parts: a program larger than one chunk of the sink, the
     * name, and unlock=1; then the same without unlock. */
    {
        size_t cap = 64 * 1024, o = 0;
        char *program = malloc(cap);
        o += (size_t)snprintf(program + o, cap - o, "G21\nG91\n");
        for (int i = 0; i < 400; i++)
            o += (size_t)snprintf(program + o, cap - o, "G1 X%d.%03d F1200 ; step %d\n", i % 7, i, i);
        const char *with_unlock[] = { "name", "ftest", "unlock", "1", NULL };
        int st = send_form(with_unlock, program, out, sizeof(out));
        CHECK(st == 200, "the form -> %d %s", st, out);
        CHECK(started.calls == 1, "the start was called %d times", started.calls);
        CHECK(!strcmp(started.name, "ftest"), "the start got the name '%s'", started.name);
        CHECK(started.unlock == 1, "the start got unlock=%d with unlock=1", started.unlock);
        CHECK(!started.under, "the start ran under a hold");
        CHECK(started.len == o && started.program && !memcmp(started.program, program, o),
              "the start got %zu bytes of the %zu sent, or other bytes", started.len, o);
        const char *without_unlock[] = { "name", "ftest", NULL };
        st = send_form(without_unlock, program, out, sizeof(out));
        CHECK(st == 200 && started.calls == 2 && started.unlock == 0,
              "the form without unlock -> %d, %d starts, unlock=%d", st, started.calls, started.unlock);
        /* A program that names no bound gets the longest a client may
         * name; one that names a bound keeps it. */
        CHECK(started.run_timeout_s == 86400.0, "a program with no timeout_s was given %.0f s",
              started.run_timeout_s);
        const char *bounded[] = { "name", "ftest", "timeout_s", "120", NULL };
        st = send_form(bounded, program, out, sizeof(out));
        CHECK(st == 200 && started.run_timeout_s == 120.0, "timeout_s=120 -> %d, %.0f s", st, started.run_timeout_s);

        /* A second upload that begins while the first is handed to the
         * runner is refused: the first runs its own lines. */
        second_upload = 1;
        st = send_form(without_unlock, program, out, sizeof(out));
        CHECK(st == 200 && started.len == o && started.program && !memcmp(started.program, program, o),
              "the first program, with a second upload begun at its start: %d, %zu bytes of %zu", st,
              started.len, o);
        free(program);
        printf("the form parts: %zu bytes, the name and unlock reach the start\n", o);
    }

    /* A missing name. */
    {
        int calls = started.calls;
        const char *none[] = { "unlock", "1", NULL };
        int st = send_form(none, "G21\nG91\nG1 X5 F1200\n", out, sizeof(out));
        CHECK(st == 400, "no name -> %d %s", st, out);
        CHECK(started.calls == calls, "a program with no name was started");
        CHECK(!is_staged(), "a program with no name stays staged");
        printf("a missing name: %d\n", st);
    }

    /* A $ line, refused with its line number. */
    {
        int calls = started.calls;
        const char *named[] = { "name", "ftest", NULL };
        int st = send_form(named, "G21\n$X\nG91\nG1 X5 F1200\n", out, sizeof(out));
        CHECK(st == 400 && strstr(out, "line 2"), "a $ line on line 2 -> %d %s", st, out);
        CHECK(started.calls == calls, "a program with a $ line was started");
        CHECK(!is_staged(), "a program with a $ line stays staged");
        printf("a $ line: %d %s\n", st, out);
    }

    /* A malformed boundary: the body's delimiter is not the declared one. */
    {
        int calls = started.calls;
        const char *named[] = { "name", "ftest", NULL };
        size_t len;
        char *b = form("someOtherBoundary", named, "G21\nG91\nG1 X5 F1200\n", &len);
        int st = post("multipart/form-data; boundary=" BOUNDARY, b, len, out, sizeof(out));
        free(b);
        CHECK(st == 400, "a malformed boundary -> %d %s", st, out);
        CHECK(started.calls == calls, "a program behind a malformed boundary was started");
        CHECK(!is_staged(), "a program behind a malformed boundary was staged");
        printf("a malformed boundary: %d\n", st);
    }

    /* An upload is its request's own thread's: a request on another thread
     * whose structure sits at an abandoned upload's address adopts none of
     * its bytes. */
    {
        int calls = started.calls;
        struct _u_request *r = &second_req;
        jobpost_sink(r, "program", "G21\nG1 X12", 0, 10);          /* staged here, then abandoned */
        pthread_t t;
        pthread_create(&t, NULL, stranger_post, r);
        pthread_join(t, NULL);
        CHECK(stranger_status == 400 && started.calls == calls,
              "a request on another thread adopted an abandoned upload: %d, %d starts", stranger_status,
              started.calls - calls);
        printf("an abandoned upload: %d\n", stranger_status);
    }

    ulfius_stop_framework(&inst);
    ulfius_clean_instance(&inst);
    free(started.program);
    rmdir(rundir);
    if (fails) {
        printf("jobpost_test: %d FAILED\n", fails);
        return 1;
    }
    printf("jobpost_test: OK\n");
    return 0;
}
