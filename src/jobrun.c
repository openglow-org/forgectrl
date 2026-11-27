/*
 * jobrun.c - the job runner: one job at a time with the daemon as its sender
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See jobrun.h. One mutex holds the runner's own state; it is never held
 * across a client's callback or across the run, and the lease is taken
 * and given back inside it, so "no run is active" and "the runner holds
 * no lease" are one fact to anybody who asks. The Grbl socket's keep is
 * asked for and given back with the mutex let go (the controller's port
 * answers in its own time), always while the run is active, so no other
 * run begins between. A client's own mutex comes before the runner's,
 * never after.
 */
#define _GNU_SOURCE
#include "jobrun.h"
#include "fflog.h"
#include "lease.h"
#include "testhook.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { JR_IDLE, JR_RUNNING, JR_DONE, JR_FAILED };

int (*jobrun_sender_out)(char *err, size_t elen);
void (*jobrun_sender_in)(void);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int active;                      /* a run plays, started or on a caller's thread */
static int lease_held;                  /* and its lease is still the runner's */
static int sender_kept;                 /* and the Grbl socket is kept to this host for it */
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
/* The last run, one the controller took a line of, ended with the
 * runner's own soft reset, and the controller showed it after the run:
 * the sender generation it published once the run's connection was gone,
 * and which controller it was. */
static struct {
    int set;
    long gen;
    long ctl;
    long alarm3;                        /* the controller's own alarm-3 count it showed then */
} stop_mark;
/* The controller's count of the alarm-3 raises it made itself (its
 * emission witness, a refused arm: grbl.state "alarm3_raised"), read as
 * the run began; -1 when it cannot be read. A stop is marked as the
 * runner's only when the document that shows it carries the same count:
 * a raise of the controller's own during the run or inside the end's read
 * reads exactly like the alarm 3 of a soft reset in motion, and is not the
 * runner's to clear. */
static long run_alarm3 = -1;

static long (*jobrun_controller)(void);

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

static long controller_id(void)
{
    return jobrun_controller ? jobrun_controller() : 0;
}

/* A whole number after key in the controller's state document, or -1. */
static long state_number(const char *body, const char *key)
{
    const char *p = strstr(body, key);
    if (!p)
        return -1;
    p += strlen(key);
    char *end;
    long v = strtol(p, &end, 10);
    return end == p || v < 0 ? -1 : v;
}

/* The state document reads the alarm a soft reset in motion leaves. */
static int reads_alarm3(const char *body)
{
    return strstr(body, "\"state\":\"Alarm\"") && state_number(body, "\"alarm\":") == 3;
}

/* After a run's own soft reset: the sender generation of the first
 * document that shows that stop, alarm 3 with no connection on the
 * socket, within JOBRUN_STOP_SEEN_S. 0, or -1 when the state cannot be
 * read or never shows it (a stop at rest raises no alarm of its own), or
 * when the controller's own alarm-3 count moved since the run began (its
 * emission witness under the run or inside the read). Asked while
 * the run still keeps the Grbl socket to this host, so no sender from the
 * network can come between. After a lost connection the alarm is raised
 * only once the controller has read the ^X of the reset connection, so
 * the document that shows it with no connection comes after that
 * connection's drop. */
static int stop_seen(long *gen, long *alarm3)
{
    char body[JOBSTREAM_STATE_MAX];
    struct timespec tick = { 0, 20 * 1000000L };
    if (run_alarm3 < 0)
        return -1;
    for (double t0 = mono_s(); mono_s() - t0 < JOBRUN_STOP_SEEN_S; nanosleep(&tick, NULL)) {
        if (jobstream_grbl_state(body, sizeof(body)) != 0)
            return -1;
        /* An alarm 3 the controller raised itself since the run began,
         * under the run or in this read: whatever the document shows, the
         * alarm that stands is not only the runner's. */
        if (state_number(body, "\"alarm3_raised\":") != run_alarm3)
            return -1;
        if (strstr(body, "\"connected\":false") && reads_alarm3(body)) {
            *alarm3 = state_number(body, "\"alarm3_raised\":");
            return (*gen = state_number(body, "\"generation\":")) < 0 ? -1 : 0;
        }
    }
    return -1;
}

/* The runner's own stop still stands: the controller that showed it reads
 * the alarm a soft reset in motion leaves (alarm 3), no session has had
 * the Grbl socket since (the same sender generation), the controller has
 * raised no alarm 3 of its own since (the same alarm3_raised: its emission
 * witness reads exactly like the runner's alarm while that stands), and
 * nothing of this daemon's has cleared or changed the alarm since
 * (jobrun_forget_stop: the port's home, release and energize). Anything
 * else (another alarm,
 * another sender's session, a controller started since, a state that
 * cannot be read) is not the runner's to clear. Called with mu held. */
static int stop_stands_locked(void)
{
    char body[JOBSTREAM_STATE_MAX];
    if (!stop_mark.set || jobstream_grbl_state(body, sizeof(body)) != 0)
        return 0;
    return controller_id() == stop_mark.ctl && reads_alarm3(body) &&
           state_number(body, "\"generation\":") == stop_mark.gen &&
           state_number(body, "\"alarm3_raised\":") == stop_mark.alarm3;
}

void jobrun_forget_stop(void)
{
    pthread_mutex_lock(&mu);
    stop_mark.set = 0;
    pthread_mutex_unlock(&mu);
}

void jobrun_wire(long (*controller)(void), void (**alarm_touched)(void))
{
    jobrun_controller = controller;
    if (alarm_touched)
        *alarm_touched = jobrun_forget_stop;
}

/* ------------------------------------------------------------ the run */

/* The Grbl socket kept to this host for the run that begins. The port
 * answers in its own time, so it is asked with the mutex let go; active
 * is set already, so nothing else begins meanwhile. Called with mu held. */
static int sender_take_locked(char *err, size_t elen)
{
    if (!jobrun_sender_out)
        return 0;
    pthread_mutex_unlock(&mu);
    int rc = jobrun_sender_out(err, elen);
    pthread_mutex_lock(&mu);
    if (rc == 0)
        sender_kept = 1;
    return rc;
}

/* And given back, while active still says the run is the runner's: the
 * next run's keep can never meet this run's give. Called with mu held. */
static void sender_give_locked(void)
{
    if (!sender_kept)
        return;
    sender_kept = 0;
    if (jobrun_sender_in) {
        pthread_mutex_unlock(&mu);
        jobrun_sender_in();
        pthread_mutex_lock(&mu);
    }
}

/* A start that did not come to a run: the socket and the lease back.
 * Called with mu held. */
static void unbegin_locked(void)
{
    sender_give_locked();
    lease_release(holder);
    lease_held = active = 0;
}

/* The way in: the lease as a sender, then the Grbl socket kept to this
 * host. The record of the last job stands until record_open_locked(), so
 * a refused start changes nothing anybody reads. Called with mu held. */
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
    if (sender_take_locked(err, elen) != 0) {
        unbegin_locked();
        return -1;
    }
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
    /* Before any of the run's lines goes out: the baseline its stop is
     * judged against (stop_seen). */
    char body[JOBSTREAM_STATE_MAX];
    run_alarm3 = jobstream_grbl_state(body, sizeof(body)) == 0
        ? state_number(body, "\"alarm3_raised\":") : -1;
}

/* The way out: the counters kept, the socket and the lease back if the
 * client has not taken them back already, the state published last. */
static void end(int rc, const jobstream_run_t *run, const char *err)
{
    char who[LEASE_OWNER_MAX];
    /* Seen before the socket goes back: the stop's mark is the
     * controller's own word, with no other session yet possible. Only a
     * run the controller took a line of (an ok to it): a controller that
     * answers nothing, or only its alarm lock, may be in an alarm that
     * stood before the run, which a soft reset at rest keeps. The
     * controller's identity is read before the state, and the next
     * program's check compares it: a controller started inside the read
     * is not the one marked. No identity (nothing wired), no mark. */
    long gen = -1, alarm3 = -1;
    long ctl = controller_id();
    int stopped = rc != 0 && run->own_stop && run->acked > 0 && ctl > 0 && stop_seen(&gen, &alarm3) == 0;
    pthread_mutex_lock(&mu);
    sender_give_locked();
    last = *run;
    last_elapsed = mono_s() - t_started;
    stop_mark.set = stopped;
    stop_mark.gen = gen;
    stop_mark.ctl = ctl;
    stop_mark.alarm3 = alarm3;
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
    /* The witnesses of every job that lit the tube, the head's beam
     * sensor among them: nothing requires the sensor, so its output is
     * kept here, where a head that reads differently shows. */
    if (run->lit)
        fflog(LOG_INFO, "jobrun: %s emission: tube current peak %ld, LASER_ON samples %ld, "
              "beam sensor dark %ld, peak %ld (rise %ld)", who, run->hv_max, run->lon_max,
              run->tp_base, run->tp_max,
              run->tp_base >= 0 && run->tp_max > run->tp_base ? run->tp_max - run->tp_base : 0);
}

void jobrun_lease_back(void)
{
    pthread_mutex_lock(&mu);
    if (active && lease_held) {
        sender_give_locked();
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
    /* The limit is on what goes out: comments gone, the ends trimmed. */
    if (n > JOBRUN_LINE_MAX) {
        *why = "is longer than the sender takes";
        return -1;
    }
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
    char *raw = NULL, out[RAW_MAX];
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

/* The controller's number reader (the core's read_float), on a line with
 * no spaces: a sign, then digits with at most one point, the digits past
 * the ninth significant one dropped. The characters read, 0 for no
 * number. */
static int core_number(const char *s, float *out)
{
    const char *p = s;
    int neg = 0, point = 0, digits = 0, any = 0, exp = 0;
    uint32_t v = 0;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            any = 1;
            if (*p != '0' || v)
                digits++;
            if (digits <= 9) {
                if (point)
                    exp--;
                v = v * 10 + (uint32_t)(*p - '0');
            } else if (!point)
                exp++;
        } else if (*p == '.' && !point)
            point = 1;
        else
            break;
    }
    if (!any)
        return 0;
    float f = (float)v;
    if (f != 0.0f) {
        while (exp <= -2) {
            f *= 0.01f;
            exp += 2;
        }
        if (exp < 0)
            f *= 0.1f;
        else
            for (; exp > 0; exp--)
                f *= 10.0f;
    }
    *out = neg ? -f : f;
    return (int)(p - s);
}

/* The P words of a line with no spaces, as the parser reads them: how
 * many, and the last one's value. */
static int line_p(const char *w, float *val)
{
    int count = 0;
    for (const char *p = w; *p; p++) {
        float v;
        if (*p == 'P' && core_number(p + 1, &v)) {
            count++;
            *val = v;
        }
    }
    return count;
}

const char *jobrun_line_kept_word(const char *line, int tray)
{
    char w[RAW_MAX];
    size_t n = 0;
    for (const char *p = line; *p && n + 1 < sizeof(w); p++)
        if (*p > ' ')
            w[n++] = (char)toupper((unsigned char)*p);
    w[n] = '\0';
    for (size_t i = 0; i < n;) {
        char letter = w[i++];
        float v;
        int used = core_number(w + i, &v);
        if (!used || (letter != 'G' && letter != 'M'))
            continue;
        i += (size_t)used;
        /* The command's number and its fraction in hundredths, as the
         * parser splits them. */
        int whole = (int)truncf(v);
        int frac = (int)roundf(fabsf(100.0f * (v - (float)whole)));
        if (letter == 'G' && whole == 10)
            return "G10";
        if (letter == 'G' && whole == 20)
            return "G20";
        if (letter == 'G' && whole == 28 && frac == 10)
            return "G28.1";
        if (letter == 'G' && whole == 30 && frac == 10)
            return "G30.1";
        if (letter == 'G' && whole == 43)
            return "G43";
        if (letter == 'G' && whole == 51)
            return "G51";
        if (letter == 'G' && whole == 92)
            return "G92";
        if (letter == 'M' && whole == 103) {
            /* The crumb tray's mode is taken only as the mode already in
             * force: M103 itself, one P of exactly 0 (in) or 1 (out), and
             * that P the marker's; a marker that cannot be looked at (-1)
             * matches no P. The controller's M103 to the mode it is in
             * changes nothing. */
            float pv = -1.0f;
            if (frac != 0 || line_p(w, &pv) != 1 || (pv != 0.0f && pv != 1.0f) || (int)pv != tray)
                return "M103";
        }
    }
    return NULL;
}

int jobrun_program_check_package(const char *path, int tray, char *err, size_t elen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, elen, "no program was received");
        return -1;
    }
    char raw[RAW_MAX], out[RAW_MAX];
    int row = 0, rc = 0;
    while (rc == 0 && fgets(raw, sizeof(raw), f)) {
        const char *why = NULL, *word;
        row++;
        if (line_clean(raw, out, sizeof(out), &why) > 0 && (word = jobrun_line_kept_word(out, tray))) {
            if (!strcmp(word, "M103"))
                snprintf(err, elen, "line %d has M103, which a package's program may give only as the tray's mode "
                                    "in force (M103 P0 with the tray in, M103 P1 with it out)%s", row,
                         tray < 0 ? ", and the tray's mode cannot be read" : "");
            else
                snprintf(err, elen, "line %d has %s, which changes what the controller keeps for later jobs: "
                                    "a package's program is in millimeters, moves from where the head is, "
                                    "and sets no offset, stored position or scale", row, word);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

int (*jobrun_tray_mode)(void);

typedef struct {
    FILE *f;                            /* open, and its name already gone */
    int unlock;                         /* the runner's $X is still to go out */
    int ended;                          /* the last line out was the program's end */
    int closed;                         /* and the runner's own M2 has gone out */
    int package;                        /* a package's program: its M103 is judged as it goes out */
    int row;                            /* the program's lines read so far */
} program_t;
static program_t prog;                  /* written under mu, and only while no run is active */

static int program_gen(void *ctx, char *buf, size_t len)
{
    program_t *p = ctx;
    char raw[RAW_MAX], clean[RAW_MAX];
    if (p->unlock) {
        p->unlock = 0;
        snprintf(buf, len, "$X");
        return 1;
    }
    while (p->f && fgets(raw, sizeof(raw), p->f)) {
        const char *why = NULL;
        p->row++;
        int n = line_clean(raw, clean, sizeof(clean), &why);
        if (n < 0) {
            snprintf(buf, len, "a line of the program %s", why);
            return -1;
        }
        if (n == 0)
            continue;
        /* A package's M103 passed the post's check against the tray's mode
         * then; it goes out only if that is still the mode in force, read
         * again now, under the run's lease and with the sender kept out.
         * Otherwise the run ends here, the line not sent. */
        if (p->package && jobrun_line_kept_word(clean, -1)) {
            int mode = jobrun_tray_mode ? jobrun_tray_mode() : -1;
            const char *word = jobrun_line_kept_word(clean, mode);
            if (word) {
                snprintf(buf, len, "line %d has %s, and the tray's mode in force is not the one it gives%s",
                         p->row, word, mode < 0 ? " (the tray's mode cannot be read)" : "");
                return -1;
            }
        }
        snprintf(buf, len, "%s", clean);
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
 * second of it. "done" is said when the head has stopped: the state reads
 * idle, or fault or underrun (nothing left to play, said in its own
 * words). A state that cannot be read is not idle: the wait goes on to
 * its limit and says so. */
static void kernel_idle_wait(double limit_s)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    char path[192], text[24];
    struct timespec tick = { 0, 20 * 1000000L };
    int unreadable = 0;
    snprintf(path, sizeof(path), "%s/cnc/state", r ? r : "/sys/glowforge");
    for (double t0 = mono_s(); mono_s() - t0 < limit_s; nanosleep(&tick, NULL)) {
        FILE *f = fopen(path, "r");
        int got = f && fgets(text, sizeof(text), f);
        if (f)
            fclose(f);
        unreadable = !got;
        if (!got)
            continue;
        if (!strncmp(text, "idle", 4))
            return;
        if (!strncmp(text, "fault", 5) || !strncmp(text, "underrun", 8)) {
            text[strcspn(text, "\n")] = '\0';
            fflog(LOG_WARNING, "jobrun: the kernel reads %s at the program's end", text);
            return;
        }
    }
    if (unreadable)
        fflog(LOG_WARNING, "jobrun: the kernel's state could not be read %.0f s after the program's end",
              limit_s);
    else
        fflog(LOG_WARNING, "jobrun: the kernel was still playing %.0f s after the program's end", limit_s);
}

/* Once the run keeps the Grbl socket (no session can come between this
 * and the $X): the unlock asked for goes out only where the runner's own
 * stop still stands. Called with mu held. */
static int program_ready(void *ctx, char *err, size_t elen)
{
    program_t *p = ctx;
    (void)err;
    (void)elen;
    if (p->unlock && !stop_stands_locked()) {
        p->unlock = 0;
        fflog(LOG_INFO, "jobrun: %s asked for the unlock, and the controller is not in the alarm "
                        "the runner's own stop left: no $X", holder);
    }
    return 0;
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

int jobrun_program_start(const char *path, const char *name, const char *under, double lit_timeout_s,
                         double run_timeout_s, int unlock, char *err, size_t elen)
{
    return jobrun_program_start_as(path, name, under, lit_timeout_s, run_timeout_s, unlock, 0, err, elen);
}

int jobrun_program_start_as(const char *path, const char *name, const char *under, double lit_timeout_s,
                            double run_timeout_s, int unlock, int package, char *err, size_t elen)
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
        .under = under,
        /* A program that must light is judged by the witnesses, so it gets
         * the full rate: at a fifth of it a burn shorter than 200 ms can
         * fall between two samples and fail the job as dark. A program
         * with no such claim may run for an hour, and only the summary of
         * its witnesses is ever read. */
        .stream = { .gen = program_gen, .ctx = &prog, .wait_timeout_s = lit_timeout_s,
                    .run_timeout_s = run_timeout_s, .sample_div = lit_timeout_s > 0 ? 1 : 5 },
        .ready = program_ready,
        .done = program_done,
    };
    pthread_mutex_lock(&mu);
    int rc = -1;
    if (active)
        busy_words(err, elen);
    else {
        memset(&prog, 0, sizeof(prog));
        prog.f = f;
        /* The $X clears the alarm the runner's own soft reset left, and
         * no other: an alarm of anything else's stands, and the program's
         * first line meets it (program_ready decides). */
        prog.unlock = unlock;
        prog.package = package;
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

int jobrun_program_abort_of(const char *name, char *err, size_t elen)
{
    char who[LEASE_OWNER_MAX], mine[LEASE_OWNER_MAX];
    snprintf(mine, sizeof(mine), "job:%s", name ? name : "");
    pthread_mutex_lock(&mu);
    int on = active, program = is_program;
    snprintf(who, sizeof(who), "%s", holder);
    pthread_mutex_unlock(&mu);
    /* Every refusal is built here from the one read: the stop below is
     * the only one, and it stops the run only while job:<name> holds it
     * (a run started since the read is somebody else's, and runs on). */
    if (!on) {
        snprintf(err, elen, "no job is running");
        return -1;
    }
    if (!program || strcmp(who, mine) != 0) {
        snprintf(err, elen, "the running job is not this package's");
        return -1;
    }
    jobrun_stop(mine);
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
