/*
 * jobrun.c - the job runner: one job at a time with the daemon as its sender
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See jobrun.h. One mutex holds the runner's own state; it is never held
 * across a client's callback or across the run, and the lease is taken
 * and given back inside it, so "no run is active" and "the runner holds
 * no lease" are one fact to anybody who asks. A client's own mutex comes
 * before the runner's, never after.
 */
#define _GNU_SOURCE
#include "jobrun.h"
#include "fflog.h"
#include "lease.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { JR_IDLE, JR_RUNNING, JR_DONE, JR_FAILED };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int active;                      /* a run plays, started or on a caller's thread */
static int lease_held;                  /* and its lease is still the runner's */
static char holder[LEASE_OWNER_MAX];    /* the lease owner of the run that plays */
static char owner[LEASE_OWNER_MAX];     /* the record's: the run that plays, or the last one */
static int is_program;                  /* the run is a posted program's */
static int state = JR_IDLE;
static char reason[200];
static double t_started;
static int lines;                       /* a posted program's line count; 0 for a client's run */
static jobstream_run_t *cur;            /* the live counters, while active */
static jobstream_run_t last;            /* and as they ended */
static double last_elapsed;

static pthread_t thread;
static int thread_live, joining;
static jobstream_run_t thread_run;      /* one started run at a time */
static jobrun_cfg_t started;            /* and its configuration */
static volatile int abort_req;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void busy_words(char *err, size_t elen)
{
    if (!lease_refusal(err, elen))
        snprintf(err, elen, "a job is finishing");
}

/* ------------------------------------------------------------ the run */

/* The way in: the lease as a sender. The record of the last job stands
 * until record_open_locked(), so a refused start changes nothing anybody
 * reads. Called with mu held. */
static int begin_locked(const jobrun_cfg_t *cfg, char *err, size_t elen)
{
    if (!cfg || !cfg->owner || !cfg->stream.gen) {
        snprintf(err, elen, "the job has no program");
        return -1;
    }
    if (active) {
        busy_words(err, elen);
        return -1;
    }
    if (lease_take(cfg->owner, LEASE_SENDER, cfg->under, err, elen) != 0)
        return -1;
    active = lease_held = 1;
    snprintf(holder, sizeof(holder), "%s", cfg->owner);
    return 0;
}

static void record_open_locked(jobstream_run_t *run, int program, int nlines)
{
    snprintf(owner, sizeof(owner), "%s", holder);
    is_program = program;
    lines = nlines;
    state = JR_RUNNING;
    reason[0] = '\0';
    t_started = mono_s();
    memset(run, 0, sizeof(*run));
    cur = run;
}

/* A start that did not come to a run: the lease back. Called with mu held. */
static void unbegin_locked(void)
{
    lease_release(holder);
    lease_held = active = 0;
}

/* The way out: the counters kept, the lease back if the client has not
 * taken it back already, the state published last. */
static void end(int rc, const jobstream_run_t *run, const char *err)
{
    char who[LEASE_OWNER_MAX];
    pthread_mutex_lock(&mu);
    last = *run;
    last_elapsed = mono_s() - t_started;
    cur = NULL;
    snprintf(reason, sizeof(reason), "%s", rc == 0 ? "" : err);
    snprintf(who, sizeof(who), "%s", holder);
    if (lease_held)
        lease_release(holder);
    lease_held = 0;
    state = rc == 0 ? JR_DONE : JR_FAILED;
    active = 0;
    pthread_mutex_unlock(&mu);
    if (rc == 0)
        fflog(LOG_INFO, "jobrun: %s ran to its end", who);
    else
        fflog(LOG_WARNING, "jobrun: %s failed: %s", who, err);
}

void jobrun_lease_back(void)
{
    pthread_mutex_lock(&mu);
    if (active && lease_held) {
        lease_release(holder);
        lease_held = 0;
    }
    pthread_mutex_unlock(&mu);
}

int jobrun_sync(const jobrun_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen)
{
    pthread_mutex_lock(&mu);
    int rc = begin_locked(cfg, err, elen);
    if (rc == 0)
        record_open_locked(run, 0, 0);
    pthread_mutex_unlock(&mu);
    if (rc != 0)
        return -1;
    rc = jobstream_run(&cfg->stream, run, err, elen);
    end(rc, run, err);
    return rc;
}

static void *run_thread(void *arg)
{
    (void)arg;
    char err[200] = "";
    int rc = jobstream_run(&started.stream, &thread_run, err, sizeof(err));
    if (started.done)
        started.done(started.stream.ctx, rc, &thread_run, err);
    end(rc, &thread_run, err);
    return NULL;
}

/* Called with mu held. */
static int start_locked(const jobrun_cfg_t *cfg, int program, int nlines, char *err, size_t elen)
{
    if (joining) {
        busy_words(err, elen);
        return -1;
    }
    /* A run that has ended has given its lease back and cleared active
     * under this mutex; its thread has nothing left but to return. */
    if (!active && thread_live) {
        pthread_join(thread, NULL);
        thread_live = 0;
    }
    if (begin_locked(cfg, err, elen) != 0)
        return -1;
    if (cfg->ready && cfg->ready(cfg->stream.ctx, err, elen) != 0) {
        unbegin_locked();
        return -1;
    }
    started = *cfg;
    started.owner = holder;
    started.under = NULL;               /* begin_locked() has read it */
    abort_req = 0;
    if (!started.stream.abort_flag)
        started.stream.abort_flag = &abort_req;
    record_open_locked(&thread_run, program, nlines);
    if (pthread_create(&thread, NULL, run_thread, NULL) != 0) {
        unbegin_locked();
        cur = NULL;
        state = JR_FAILED;
        snprintf(reason, sizeof(reason), "cannot start the job's thread");
        snprintf(err, elen, "cannot start the job's thread");
        return -1;
    }
    thread_live = 1;
    return 0;
}

int jobrun_start(const jobrun_cfg_t *cfg, char *err, size_t elen)
{
    pthread_mutex_lock(&mu);
    int rc = start_locked(cfg, 0, 0, err, elen);
    pthread_mutex_unlock(&mu);
    return rc;
}

void jobrun_stop(const char *who)
{
    pthread_mutex_lock(&mu);
    int mine = thread_live && !joining && who && !strcmp(who, holder);
    if (mine) {
        joining = 1;
        if (active)
            abort_req = 1;
    }
    pthread_mutex_unlock(&mu);
    if (!mine)
        return;
    /* Joined outside the mutex: the run's way out takes it. */
    pthread_join(thread, NULL);
    pthread_mutex_lock(&mu);
    thread_live = joining = 0;
    pthread_mutex_unlock(&mu);
}

/* --------------------------------------------------- a posted program */

int jobrun_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > JOBRUN_NAME_MAX)
        return -1;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
            return -1;
    return 0;
}

/* One raw line to what goes out: comments gone ((...) and ;...), the
 * ends trimmed, a tab a space. The length (0: nothing to send), or -1
 * with the offense in why. */
static int line_clean(const char *in, char *out, size_t len, const char **why)
{
    size_t n = 0;
    int paren = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && *p != '\n'; p++) {
        unsigned char c = *p == '\t' ? ' ' : *p;
        if (c == '\r') {
            if (p[1] == '\n' || p[1] == '\0')
                continue;
            *why = "has a carriage return inside it";
            return -1;
        }
        if (c < 0x20 || c >= 0x7f) {
            *why = "has a byte that is not printable ASCII";
            return -1;
        }
        if (paren) {
            if (c == ')')
                paren = 0;
            continue;
        }
        if (c == '(') {
            paren = 1;
            continue;
        }
        if (c == ';')
            break;
        if (c == '?' || c == '!' || c == '~') {
            *why = "has a realtime character (? ! ~) outside a comment";
            return -1;
        }
        if (n == 0 && c == ' ')
            continue;
        if (n + 1 >= len) {
            *why = "is longer than the sender takes";
            return -1;
        }
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    if (out[0] == '$') {
        *why = "is a $ command: the controller's system commands are not a job's to send";
        return -1;
    }
    if (!strcmp(out, "%"))
        out[0] = '\0', n = 0;           /* a tape mark: not a line */
    return (int)n;
}

/* The program's end as the sender wants it: M2, which the controller
 * acknowledges only once every buffered motion has played. */
static int is_end(const char *l)
{
    return (l[0] == 'M' || l[0] == 'm') && l[1] == '2' && (l[2] == '\0' || l[2] == ' ');
}

#define RAW_MAX 1024                    /* one line of the file, its newline included */

int jobrun_program_check(const char *path, int *nlines, char *err, size_t elen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, elen, "no program was received");
        return -1;
    }
    char *raw = NULL, out[JOBRUN_LINE_MAX + 2];
    size_t cap = 0;
    ssize_t got;
    int row = 0, kept = 0, rc = 0;
    while (rc == 0 && (got = getline(&raw, &cap, f)) >= 0) {
        row++;
        const char *why = NULL;
        int n = -1;
        if ((size_t)got != strlen(raw))
            why = "has a byte that is not printable ASCII";     /* a NUL */
        else if (got >= RAW_MAX)
            why = "is longer than the sender takes";
        else
            n = line_clean(raw, out, sizeof(out), &why);
        if (n < 0) {
            snprintf(err, elen, "line %d %s", row, why);
            rc = -1;
        } else if (n > 0)
            kept++;
    }
    free(raw);
    fclose(f);
    if (rc == 0 && kept == 0) {
        snprintf(err, elen, "the program has no lines");
        rc = -1;
    }
    if (rc == 0 && nlines)
        *nlines = kept;
    return rc;
}

typedef struct {
    FILE *f;                            /* open, and its name already gone */
    int unlock;                         /* the runner's $X is still to go out */
    int ended;                          /* the last line out was the program's end */
    int closed;                         /* and the runner's own M2 has gone out */
} program_t;
static program_t prog;                  /* written under mu, and only while no run is active */

static int program_gen(void *ctx, char *buf, size_t len)
{
    program_t *p = ctx;
    char raw[RAW_MAX];
    if (p->unlock) {
        p->unlock = 0;
        snprintf(buf, len, "$X");
        return 1;
    }
    while (p->f && fgets(raw, sizeof(raw), p->f)) {
        const char *why = NULL;
        int n = line_clean(raw, buf, len < JOBRUN_LINE_MAX + 2 ? len : JOBRUN_LINE_MAX + 2, &why);
        if (n < 0) {
            snprintf(buf, len, "a line of the program %s", why);
            return -1;
        }
        if (n == 0)
            continue;
        p->ended = is_end(buf);
        return 1;
    }
    if (p->closed || p->ended)
        return 0;
    p->closed = 1;
    snprintf(buf, len, "M2");
    return 1;
}

/* The controller acknowledges M2 when its planner is empty; the pulse
 * engine still has the last of the ring to play, a few tenths of a
 * second of it. "done" is said when the head has stopped. */
static void kernel_idle_wait(double limit_s)
{
    const char *r = getenv("GF_SYSFS_ROOT");
    char path[192], text[24];
    struct timespec tick = { 0, 20 * 1000000L };
    snprintf(path, sizeof(path), "%s/cnc/state", r && *r ? r : "/sys/glowforge");
    for (double t0 = mono_s(); mono_s() - t0 < limit_s; nanosleep(&tick, NULL)) {
        FILE *f = fopen(path, "r");
        if (!f)
            return;
        int idle = !fgets(text, sizeof(text), f) || !strncmp(text, "idle", 4);
        fclose(f);
        if (idle)
            return;
    }
    fflog(LOG_WARNING, "jobrun: the kernel was still playing %.0f s after the program's end", limit_s);
}

static void program_done(void *ctx, int rc, const jobstream_run_t *run, const char *err)
{
    program_t *p = ctx;
    (void)run;
    (void)err;
    if (rc == 0)
        kernel_idle_wait(5.0);
    if (p->f)
        fclose(p->f);
    p->f = NULL;
}

int jobrun_program_start(const char *path, const char *name, double lit_timeout_s,
                         double run_timeout_s, int unlock, char *err, size_t elen)
{
    char who[LEASE_OWNER_MAX];
    int n = 0;
    FILE *f = NULL;
    if (jobrun_name_ok(name) != 0)
        snprintf(err, elen, "name is 1 to %d of letters, digits, '.', '_' and '-'", JOBRUN_NAME_MAX);
    else if (jobrun_program_check(path, &n, err, elen) == 0 && !(f = fopen(path, "r")))
        snprintf(err, elen, "no program was received");
    /* Open, the file needs no name: the next upload stages under the
     * same one, and must not write under this run. */
    unlink(path);
    if (!f)
        return -1;
    snprintf(who, sizeof(who), "job:%s", name);
    jobrun_cfg_t cfg = {
        .owner = who,
        /* A program that must light is judged by the witnesses, so it gets
         * the full rate: at a fifth of it a burn shorter than 200 ms can
         * fall between two samples and fail the job as dark. A program
         * with no such claim may run for an hour, and only the summary of
         * its witnesses is ever read. */
        .stream = { .gen = program_gen, .ctx = &prog, .wait_timeout_s = lit_timeout_s,
                    .run_timeout_s = run_timeout_s, .sample_div = lit_timeout_s > 0 ? 1 : 5 },
        .done = program_done,
    };
    pthread_mutex_lock(&mu);
    int rc = -1;
    if (active)
        busy_words(err, elen);
    else {
        memset(&prog, 0, sizeof(prog));
        prog.f = f;
        prog.unlock = unlock;
        rc = start_locked(&cfg, 1, n, err, elen);
        if (rc != 0)
            prog.f = NULL;
    }
    pthread_mutex_unlock(&mu);
    if (rc != 0) {
        fclose(f);
        return -1;
    }
    fflog(LOG_INFO, "jobrun: %s started (%d lines)", who, n);
    return 0;
}

int jobrun_program_abort(char *err, size_t elen)
{
    char who[LEASE_OWNER_MAX];
    pthread_mutex_lock(&mu);
    int on = active, program = is_program;
    snprintf(who, sizeof(who), "%s", holder);
    pthread_mutex_unlock(&mu);
    if (!on) {
        snprintf(err, elen, "no job is running");
        return -1;
    }
    if (!program) {
        char words[LEASE_OWNER_MAX + 32];
        lease_words(who, words, sizeof(words));
        snprintf(err, elen, "%s is running: stop it where it was started", words);
        return -1;
    }
    jobrun_stop(who);
    return 0;
}

/* ---------------------------------------------------------- the record */

static void json_text(char *buf, size_t len, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = (char)c;
        } else if (c < 0x20)
            n += (size_t)snprintf(buf + n, len - n, "\\u%04x", c);
        else
            buf[n++] = (char)c;
    }
    buf[n] = '\0';
}

int jobrun_status_json(char *buf, size_t len)
{
    static const char *names[] = { "idle", "running", "done", "failed" };
    char why[2 * sizeof(reason)];
    pthread_mutex_lock(&mu);
    const jobstream_run_t *r = cur ? cur : &last;
    long tp_delta = r->tp_base >= 0 && r->tp_max > r->tp_base ? r->tp_max - r->tp_base : 0;
    json_text(why, sizeof(why), reason);
    int n = snprintf(buf, len,
        "{\"state\":\"%s\",\"owner\":\"%s\",\"program\":%s,\"lines\":%d,\"sent\":%d,\"acked\":%d,"
        "\"elapsed_s\":%.0f,\"lit\":%s,\"emission\":{\"samples\":%d,\"hv_max\":%ld,\"laser_on_samples\":%ld,"
        "\"thermopile_delta\":%ld,\"lit_s\":%.1f},\"reason\":\"%s\"}",
        names[state], state == JR_IDLE ? "" : owner, is_program ? "true" : "false", lines,
        r->sent, r->acked, active ? mono_s() - t_started : last_elapsed,
        r->lit ? "true" : "false", r->samples, r->hv_max, r->lon_max, tp_delta,
        r->lit ? r->t_last_lit - r->t_first_lit : 0.0, why);
    pthread_mutex_unlock(&mu);
    return n > 0 && (size_t)n < len ? 0 : -1;
}
