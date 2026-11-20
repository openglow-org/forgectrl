/*
 * btnres.c - the machine's button reserved for a prompt
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The machine's one button is three things: the consent that arms a laser
 * job, the pause and resume of a running one, and the proof of presence
 * the panel's guarded actions and the Setup checks ask for. A job that
 * waits for its press, or that is held inside its armed window, takes any
 * press as its own, whoever asked for it. So a prompt that asks for the
 * button reserves it first, for as long as it stands: the cooling engine
 * then blocks fire, withholds the armed window's acknowledgment and holds
 * an open window (cool.h, cool_button_reserved), and the controllers'
 * arm, re-arm and resume refuse. Every prompt asks for the press only once
 * the reservation has stood on file long enough for both controllers to
 * have read it (BTNRES_READY_S): the panel's dialog through POST
 * /button/reserve (main.c), a package's check prompt in wizpkg.c.
 *
 * A package's check reserves the button against every job but that
 * package's own, so its own burn still arms on the press the check asks
 * for: only while the package keeps the machine for its own jobs
 * (btnres_pkg_holds), where no other party's job can be the one waiting.
 *
 * A package itself gets a press in one way: it asks for one (btnres_ask,
 * POST /button/ask). The ask is refused while anybody else waits on the
 * button, and from then on it is a reservation like the others, against
 * every job, the package's own included: a press handed to a package is
 * no job's consent. The table of reservations and the ask are one state
 * under one lock, and the engine publishes from that table, so the ask is
 * in force exactly when the controllers have read a verdict that carries
 * it. Who else waits on the button is asked three times: when the ask is
 * filed, when it comes in force, and at the press itself, where an ask
 * that is refused ends with the press unsaid: one press is one party's.
 * The reservation stands WIZPKG_LINGER_S after a press either way, said
 * or unsaid, so a second press made for the same prompt arms nothing. A
 * call that keeps a standing ask does not extend it while a controller
 * reports its armed session (btnres_session_reported): the job that came
 * to wait under the ask gets the button when the ask's time runs out.
 * A package the operator turned off or removed on the control panel asks
 * for nothing until it is turned on again (btnres_ask_bar).
 *
 * Each prompt holds a reservation of its own, named for it: one ended
 * ends no other prompt's. Every reservation is bounded: a renewal moves
 * its end, and nothing outlives BTNRES_TTL_MAX_S past its last renewal.
 */
#define _GNU_SOURCE
#include "btnres.h"
#include "wizpkg.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int (*btnres_pkg_holds)(const char *pkg) = NULL;
int (*btnres_ask_refused)(const char *pkg, char *why, size_t len) = NULL;
int (*btnres_daemon_waits)(void) = NULL;
int (*btnres_session_reported)(void) = NULL;
double (*btnres_on_file_s)(void) = NULL;
int (*btnres_button)(void) = NULL;

static pthread_mutex_t res_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char who[BTNRES_WHO_MAX];
    double until;               /* CLOCK_MONOTONIC; 0 = free */
} res_tab[BTNRES_MAX];

/* The one ask that stands, under res_mu with the table: its reservation
 * is the entry "ask:<pkg>". */
enum { ASK_NONE, ASK_WAIT, ASK_READY, ASK_PRESSED };
static struct {
    char pkg[BTNRES_PKG_MAX];
    int state;
    double began;               /* the bound of one ask counts from here */
    int released;               /* the button read released since it came in force */
    int down;                   /* pressed readings in a row since then */
    int unread;                 /* unreadable readings in a row */
} ask;
static int ask_reader;          /* the reader thread runs */

/* The packages turned off on the control panel, under res_mu: none of
 * them asks. An entry is pending from the operator's click until the
 * extension host has answered for it. With every slot in use the oldest
 * entry makes room: the extension host has long stopped that package's
 * service. */
static char bar_tab[BTNRES_BAR_MAX][BTNRES_PKG_MAX];
static unsigned long bar_seq[BTNRES_BAR_MAX], bar_next;
static int bar_pend[BTNRES_BAR_MAX];

static int bar_find_locked(const char *pkg)
{
    for (int i = 0; i < BTNRES_BAR_MAX; i++)
        if (bar_seq[i] && !strcmp(bar_tab[i], pkg))
            return i;
    return -1;
}

double (*btnres_clock)(void) = NULL;

static double res_mono(void)
{
    struct timespec ts;
    if (btnres_clock)
        return btnres_clock();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A prompt's own name: 1 to BTNRES_NONCE_MAX of letters, digits, '-'
 * and '_'. */
static int res_nonce_ok(const char *s, size_t n)
{
    if (n < 1 || n > BTNRES_NONCE_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_'))
            return 0;
    return 1;
}

/* A package id as reservations carry it: no separator, quote or line. */
static int res_pkg_ok(const char *s, size_t n)
{
    return n >= 1 && n < BTNRES_PKG_MAX && !memchr(s, ':', n) && !memchr(s, '"', n) &&
           !memchr(s, '\\', n) && !memchr(s, '\n', n);
}

/* The package a reservation names ("check:<pkg>", "ask:<pkg>") into @pkg,
 * 1; 0 for the panel's ("panel:<nonce>") or a name that is none of them. */
static int res_pkg_of(const char *who, char *pkg, size_t len)
{
    const char *p;
    if (!strncmp(who, "check:", 6))
        p = who + 6;
    else if (!strncmp(who, "ask:", 4))
        p = who + 4;
    else
        return 0;
    if (strlen(p) >= len)
        return 0;
    snprintf(pkg, len, "%s", p);
    return 1;
}

/* Whose prompt @who is, in words. */
static void res_words(const char *who, char *why, size_t len)
{
    char pkg[BTNRES_PKG_MAX];
    if (!strcmp(who, BTNRES_SETUP))
        snprintf(why, len, "the machine waits for a press of the button for its own setup");
    else if (!res_pkg_of(who, pkg, sizeof(pkg)))
        snprintf(why, len, "the control panel asks for the button");
    else if (who[0] == 'c')
        snprintf(why, len, "the check of %s asks for the button", pkg);
    else
        snprintf(why, len, "the package %s asks for the button", pkg);
}

int btnres_name_ok(const char *who)
{
    if (!who || strlen(who) >= BTNRES_WHO_MAX)
        return 0;
    if (!strcmp(who, BTNRES_SETUP))
        return 1;
    if (!strncmp(who, "panel:", 6))
        return res_nonce_ok(who + 6, strlen(who + 6));
    if (!strncmp(who, "check:", 6))
        return res_pkg_ok(who + 6, strlen(who + 6));
    return 0;
}

static int res_find_locked(const char *who, double now)
{
    for (int i = 0; i < BTNRES_MAX; i++)
        if (res_tab[i].until > now && !strcmp(res_tab[i].who, who))
            return i;
    return -1;
}

/* @who's entry, standing until @until: its slot, or -1 with every slot in
 * use. */
static int res_put_locked(const char *who, double now, double until)
{
    int i = res_find_locked(who, now);
    for (int j = 0; i < 0 && j < BTNRES_MAX; j++)
        if (res_tab[j].until <= now)
            i = j;
    if (i >= 0) {
        snprintf(res_tab[i].who, sizeof(res_tab[i].who), "%s", who);
        res_tab[i].until = until;
    }
    return i;
}

/* The ask that stands is over, and its reservation with it: but the
 * linger after a press stands its time, the press unsaid. */
static void ask_drop_locked(void)
{
    char who[BTNRES_WHO_MAX];
    if (ask.state == ASK_NONE)
        return;
    snprintf(who, sizeof(who), "ask:%s", ask.pkg);
    for (int i = 0; ask.state != ASK_PRESSED && i < BTNRES_MAX; i++)
        if (!strcmp(res_tab[i].who, who))
            res_tab[i].until = 0;
    ask.state = ASK_NONE;
}

int btnres_open(const char *who, double ttl_s, char *why, size_t len)
{
    if (!btnres_name_ok(who)) {
        snprintf(why, len, "the reservation has no usable name");
        return -1;
    }
    if (!(ttl_s > 0))
        ttl_s = BTNRES_PANEL_TTL_S;
    if (ttl_s > BTNRES_TTL_MAX_S)
        ttl_s = BTNRES_TTL_MAX_S;
    double now = res_mono();
    int rc = -1;
    pthread_mutex_lock(&res_mu);
    if (res_put_locked(who, now, now + ttl_s) >= 0) {
        /* The operator's own prompt: no package's ask stands beside it,
         * so one press is never two parties'. */
        ask_drop_locked();
        rc = 0;
    } else
        snprintf(why, len, "too many prompts reserve the button");
    pthread_mutex_unlock(&res_mu);
    return rc;
}

int btnres_renew(const char *who, double ttl_s)
{
    if (!btnres_name_ok(who) || !(ttl_s > 0))
        return -1;
    if (ttl_s > BTNRES_TTL_MAX_S)
        ttl_s = BTNRES_TTL_MAX_S;
    double now = res_mono();
    pthread_mutex_lock(&res_mu);
    int i = res_find_locked(who, now);
    if (i >= 0)
        res_tab[i].until = now + ttl_s;
    pthread_mutex_unlock(&res_mu);
    return i >= 0 ? 0 : -1;
}

void btnres_close(const char *who)
{
    if (!who)
        return;
    pthread_mutex_lock(&res_mu);
    for (int i = 0; i < BTNRES_MAX; i++)
        if (!strcmp(res_tab[i].who, who))
            res_tab[i].until = 0;
    pthread_mutex_unlock(&res_mu);
}

int btnres_stands(const char *who)
{
    if (!who)
        return 0;
    pthread_mutex_lock(&res_mu);
    int is = res_find_locked(who, res_mono()) >= 0;
    pthread_mutex_unlock(&res_mu);
    return is;
}

int btnres_any(const char *prefix)
{
    size_t n = prefix ? strlen(prefix) : 0;
    double now = res_mono();
    int is = 0;
    pthread_mutex_lock(&res_mu);
    for (int i = 0; i < BTNRES_MAX && !is; i++)
        is = res_tab[i].until > now && !strncmp(res_tab[i].who, prefix, n);
    pthread_mutex_unlock(&res_mu);
    return is;
}

int btnres_withheld(char *why, size_t len)
{
    char held[BTNRES_MAX][BTNRES_WHO_MAX];
    int n = 0;
    double now = res_mono();
    pthread_mutex_lock(&res_mu);
    for (int i = 0; i < BTNRES_MAX; i++)
        if (res_tab[i].until > now)
            memcpy(held[n++], res_tab[i].who, sizeof(held[0]));    /* the same size, NUL-terminated */
    pthread_mutex_unlock(&res_mu);
    /* Asked outside the lock: the answer reads the sender claim. */
    for (int i = 0; i < n; i++) {
        char pkg[BTNRES_PKG_MAX];
        /* A check's own package alone: an ask withholds every job. */
        if (held[i][0] == 'c' && res_pkg_of(held[i], pkg, sizeof(pkg)) && btnres_pkg_holds && btnres_pkg_holds(pkg))
            continue;
        res_words(held[i], why, len);
        return 1;
    }
    return 0;
}

/* ---- a package's ask for a press ---- */

/* An ask that waits comes in force once the reservation has stood on file
 * for BTNRES_READY_S: both controllers have read it, so no job takes a
 * press from here on. A job that came to wait before they had is the one
 * the button belongs to: the ask ends, with who in @why. A press counts
 * from a release read in force, so one begun earlier is nobody's. */
static void ask_judge_locked(char *why, size_t len)
{
    if (ask.state != ASK_WAIT || !btnres_on_file_s || btnres_on_file_s() < BTNRES_READY_S)
        return;
    if (btnres_ask_refused && btnres_ask_refused(ask.pkg, why, len)) {
        ask_drop_locked();
        return;
    }
    ask.state = ASK_READY;
}

void btnres_ask_step(int button)
{
    char who[BTNRES_WHO_MAX], why[160];
    double now = res_mono();
    pthread_mutex_lock(&res_mu);
    if (ask.state != ASK_NONE) {
        snprintf(who, sizeof(who), "ask:%s", ask.pkg);
        int own = res_find_locked(who, now);
        if (own < 0)
            ask.state = ASK_NONE;       /* nobody renewed it, or its time is up */
        else if (ask.state != ASK_PRESSED && btnres_daemon_waits && btnres_daemon_waits())
            ask_drop_locked();
        else if (ask.state != ASK_PRESSED && button < 0 && ++ask.unread >= BTNRES_ASK_UNREAD_MAX)
            ask_drop_locked();          /* no press can be read: no job is withheld for one */
        else if (ask.state != ASK_PRESSED) {
            if (button >= 0)
                ask.unread = 0;
            ask_judge_locked(why, sizeof(why));
            if (ask.state != ASK_READY || button != 1) {
                if (ask.state == ASK_READY && button == 0)
                    ask.released = 1;
                ask.down = 0;
            } else if (ask.released && ++ask.down >= 2) {
                /* The press, read twice in a row. Somebody else who came
                 * to wait on the button since the ask came in force (a job
                 * that runs, this daemon's own check, the machine's lease)
                 * takes it as their own: the ask ends, the press unsaid.
                 * Else it is the package's, if the reservation is on file
                 * as it was when it came in force. Said or unsaid, the
                 * reservation stands WIZPKG_LINGER_S from here: the press
                 * was made for the package's prompt, and a second one made
                 * for it must not arm a job that came to wait meanwhile. */
                if (btnres_ask_refused && btnres_ask_refused(ask.pkg, why, sizeof(why))) {
                    ask.state = ASK_NONE;
                    res_tab[own].until = now + WIZPKG_LINGER_S;
                } else if (btnres_on_file_s && btnres_on_file_s() >= BTNRES_READY_S) {
                    ask.state = ASK_PRESSED;
                    res_tab[own].until = now + WIZPKG_LINGER_S;
                } else
                    ask.released = ask.down = 0;
            }
        }
    }
    pthread_mutex_unlock(&res_mu);
}

static void *ask_reader_thread(void *arg)
{
    const struct timespec poll = { 0, BTNRES_ASK_POLL_MS * 1000000L };
    (void)arg;
    for (;;) {
        btnres_ask_step(btnres_button ? btnres_button() : -1);
        pthread_mutex_lock(&res_mu);
        int go = ask.state != ASK_NONE;
        if (!go)
            ask_reader = 0;
        pthread_mutex_unlock(&res_mu);
        if (!go)
            return NULL;
        nanosleep(&poll, NULL);
    }
}

int btnres_ask(const char *pkg, const char **state, char *why, size_t len)
{
    char who[BTNRES_WHO_MAX];
    if (!pkg || !res_pkg_ok(pkg, strlen(pkg))) {
        snprintf(why, len, "id is a package id");
        return -2;
    }
    snprintf(who, sizeof(who), "ask:%s", pkg);
    /* The button is read for a new ask alone, outside the lock, as the
     * reader thread reads it: a call that keeps a standing ask, and one
     * of a package turned off, read nothing. */
    pthread_mutex_lock(&res_mu);
    int fresh = bar_find_locked(pkg) < 0 && !(ask.state != ASK_NONE && !strcmp(ask.pkg, pkg));
    pthread_mutex_unlock(&res_mu);
    int unreadable = fresh && btnres_button && btnres_button() < 0;
    double now = res_mono();
    int rc = 0;
    pthread_mutex_lock(&res_mu);
    int barred = bar_find_locked(pkg);
    if (barred >= 0) {
        if (bar_pend[barred])
            snprintf(why, len, "the package %s is being turned off on the control panel", pkg);
        else
            snprintf(why, len, "the package %s was turned off on the control panel: turn it on there "
                               "(off and on again, if it shows as on)", pkg);
        pthread_mutex_unlock(&res_mu);
        return -1;
    }
    int session = 0;
    int own = res_find_locked(who, now);
    int mine = ask.state != ASK_NONE && !strcmp(ask.pkg, pkg);
    if (mine && own < 0) {
        ask.state = ASK_NONE;           /* nobody renewed it, or its time is up */
        mine = 0;
    }
    /* Anybody else's reservation refuses it, by name. */
    for (int i = 0; i < BTNRES_MAX && rc == 0; i++)
        if (res_tab[i].until > now && strcmp(res_tab[i].who, who)) {
            res_words(res_tab[i].who, why, len);
            rc = -1;
        }
    if (rc != 0) {
        if (mine)
            ask_drop_locked();
    } else if (mine && ask.state == ASK_PRESSED) {
        ask.state = ASK_NONE;           /* said once; the reservation stands its linger */
        *state = "pressed";
    } else {
        if (mine) {
            /* Renewed, inside the one ask's bound: but not while a
             * controller reports its armed session. A job came to wait
             * under the ask, and the ask's time runs out for it: the
             * package is told "waiting" from this call on, since a press
             * made now would not be said to it. */
            double until = now + BTNRES_PANEL_TTL_S, last = ask.began + BTNRES_TTL_MAX_S;
            session = btnres_session_reported && btnres_session_reported();
            if (!session)
                res_tab[own].until = until < last ? until : last;
        } else if (btnres_ask_refused && btnres_ask_refused(pkg, why, len)) {
            rc = -1;
        } else if (unreadable) {
            snprintf(why, len, "the machine's button cannot be read");
            rc = -1;
        } else if (res_put_locked(who, now, now + BTNRES_PANEL_TTL_S) < 0) {
            snprintf(why, len, "too many prompts reserve the button");
            rc = -1;
        } else {
            memset(&ask, 0, sizeof(ask));
            snprintf(ask.pkg, sizeof(ask.pkg), "%s", pkg);
            ask.state = ASK_WAIT;
            ask.began = now;
            if (btnres_button && !ask_reader) {
                pthread_t th;
                pthread_attr_t at;
                pthread_attr_init(&at);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&th, &at, ask_reader_thread, NULL) == 0)
                    ask_reader = 1;
                else {
                    ask_drop_locked();
                    snprintf(why, len, "the machine's button cannot be read");
                    rc = -1;
                }
                pthread_attr_destroy(&at);
            }
        }
        if (rc == 0) {
            ask_judge_locked(why, len);
            if (ask.state == ASK_NONE)
                rc = -1;
            else
                *state = ask.state == ASK_READY && !session ? "ready" : "waiting";
        }
    }
    pthread_mutex_unlock(&res_mu);
    return rc;
}

void btnres_ask_end(const char *pkg)
{
    if (!pkg)
        return;
    pthread_mutex_lock(&res_mu);
    if (ask.state != ASK_NONE && !strcmp(ask.pkg, pkg))
        ask_drop_locked();
    pthread_mutex_unlock(&res_mu);
}

/* @pkg's entry among the barred, made if it has none (pending or not as
 * @pending says; one it has keeps its own), and its ask ended. Returns 1
 * when the entry is new. */
static int bar_enter_locked(const char *pkg, int pending)
{
    int i = bar_find_locked(pkg), is_new = i < 0;
    for (int j = 0; i < 0 && j < BTNRES_BAR_MAX; j++)
        if (!bar_seq[j])
            i = j;
    if (i < 0) {
        i = 0;
        for (int j = 1; j < BTNRES_BAR_MAX; j++)
            if (bar_seq[j] < bar_seq[i])
                i = j;
    }
    if (is_new) {
        snprintf(bar_tab[i], sizeof(bar_tab[i]), "%s", pkg);
        bar_seq[i] = ++bar_next;
        bar_pend[i] = pending;
    } else if (!pending)
        bar_pend[i] = 0;
    if (ask.state != ASK_NONE && !strcmp(ask.pkg, pkg))
        ask_drop_locked();
    return is_new;
}

int btnres_ask_bar_pending(const char *pkg)
{
    if (!pkg || !res_pkg_ok(pkg, strlen(pkg)))
        return 0;
    pthread_mutex_lock(&res_mu);
    int is_new = bar_enter_locked(pkg, 1);
    pthread_mutex_unlock(&res_mu);
    return is_new;
}

void btnres_ask_bar(const char *pkg)
{
    if (!pkg || !res_pkg_ok(pkg, strlen(pkg)))
        return;
    pthread_mutex_lock(&res_mu);
    bar_enter_locked(pkg, 0);
    pthread_mutex_unlock(&res_mu);
}

void btnres_ask_unbar(const char *pkg)
{
    if (!pkg)
        return;
    pthread_mutex_lock(&res_mu);
    int i = bar_find_locked(pkg);
    if (i >= 0)
        bar_seq[i] = 0;
    pthread_mutex_unlock(&res_mu);
}
