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
 * The panel's dialog is a consent: one press for one guarded action, of
 * one session, for one thing (btnres_consent_open). Its press is read here
 * as an ask's is, a release in force and then two readings down, never the
 * level of the button when the action's request arrives; the action takes
 * it once, under the same name, kind, subject and holder. One consent
 * stands on the machine at a time, so one press is one consent's: a second
 * holder's registration while one stands refuses both.
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
#include "testhook.h"
#include "wizpkg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long a consent's reservation stands after its press:
 * WIZPKG_LINGER_S, or a host test's shorter one (GF_CONSENT_LINGER_S). */
static double consent_linger(void)
{
    const char *v = ff_test_hook("GF_CONSENT_LINGER_S");
    return v && atof(v) > 0 ? atof(v) : WIZPKG_LINGER_S;
}

int (*btnres_pkg_holds)(const char *pkg) = NULL;
int (*btnres_ask_refused)(const char *pkg, char *why, size_t len) = NULL;
int (*btnres_daemon_waits)(void) = NULL;
int (*btnres_session_reported)(void) = NULL;
double (*btnres_on_file_s)(void) = NULL;
int (*btnres_button)(void) = NULL;
int (*btnres_consent_refused)(char *why, size_t len) = NULL;

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
static int reader;              /* the reader thread runs, for the ask or the consent */

/* The one consent that stands, under res_mu with the table: its
 * reservation is the entry "panel:<nonce>". */
enum { CONSENT_NONE, CONSENT_WAIT, CONSENT_READY, CONSENT_PRESSED };
static struct {
    char nonce[BTNRES_NONCE_MAX + 1];
    char kind[BTNRES_KIND_MAX];
    char subject[BTNRES_SUBJECT_MAX];
    char holder[BTNRES_HOLDER_MAX];
    char peer[BTNRES_PEER_MAX];
    int state;
    double began;               /* its registration: BTNRES_CONSENT_S counts from here */
    double judged;              /* when a waiting consent was last judged */
    double pressed_at;          /* the last press read for it, its own or not, 0 for none: the linger counts from here */
    char waits[160];            /* who else waits, while it waits for them */
    int released, down, unread; /* as the ask's */
} consent;

/* The nonces whose consent ended, with the words it ended in: none is
 * registered again, so a renewal on its way when the dialog closed, or a
 * refused dialog's next one, opens nothing. */
#define CONSENT_ENDED_MAX 8
static struct {
    char nonce[BTNRES_NONCE_MAX + 1];
    char why[112];
} consent_ended[CONSENT_ENDED_MAX];
static unsigned consent_ended_next;

#define CONSENT_OTHER "another consent was asked for at the same time: start it again from the control panel"
#define CONSENT_ELSE "the press was asked for something else: start it again from the control panel"
#define CONSENT_NONE_WORDS "the button was not reserved for this: start it again from the control panel"

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

/* The ask ended for the operator's own prompt (a consent, the Setup page's
 * wait, a package's check): its reservation stands WIZPKG_LINGER_S from
 * @now, as after a press, so a press the operator makes for the package's
 * prompt in that time, before its page has learned of the end, is
 * nobody's, never the prompt's (consent_others_locked keeps the consent
 * out meanwhile, wiz_press_judge the Setup's wait, ready_wait the check's
 * prompt), nor a job's (the ask's reservation withholds every job). A pressed ask keeps the linger its
 * press gave it; its "pressed", not yet collected, is said to nobody. */
static void ask_drop_lingering_locked(double now)
{
    char who[BTNRES_WHO_MAX];
    if (ask.state == ASK_NONE)
        return;
    snprintf(who, sizeof(who), "ask:%s", ask.pkg);
    for (int i = 0; ask.state != ASK_PRESSED && i < BTNRES_MAX; i++)
        if (!strcmp(res_tab[i].who, who) && res_tab[i].until > now)
            res_tab[i].until = now + WIZPKG_LINGER_S;
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
         * so one press is never two parties'. The Setup page's wait and a
         * package's check leave the ask's reservation its linger, as a
         * consent does, and do not come in force while it stands (wiz.c,
         * wiz_press_judge; wizpkg.c, ready_wait): a check withholds nothing
         * from its own package's jobs, so without the linger a press made
         * for the ask's prompt could arm one. */
        if (!strcmp(who, BTNRES_SETUP) || !strncmp(who, "check:", 6))
            ask_drop_lingering_locked(now);
        else
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

/* The reader thread, while an ask stands or a consent waits for its
 * press: one reading of the button, every BTNRES_ASK_POLL_MS, for both. */
static void *reader_thread(void *arg)
{
    const struct timespec poll = { 0, BTNRES_ASK_POLL_MS * 1000000L };
    (void)arg;
    for (;;) {
        int b = btnres_button ? btnres_button() : -1;
        btnres_ask_step(b);
        btnres_consent_step(b);
        pthread_mutex_lock(&res_mu);
        int go = ask.state != ASK_NONE || consent.state == CONSENT_WAIT || consent.state == CONSENT_READY;
        if (!go)
            reader = 0;
        pthread_mutex_unlock(&res_mu);
        if (!go)
            return NULL;
        nanosleep(&poll, NULL);
    }
}

/* Under res_mu: the reader thread, started when it is not running (and
 * when there is a button to read): 0, or -1 when it cannot be. */
static int reader_start_locked(void)
{
    if (!btnres_button || reader)
        return 0;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, reader_thread, NULL) == 0 ? 0 : -1;
    pthread_attr_destroy(&at);
    if (rc == 0)
        reader = 1;
    return rc;
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
            if (reader_start_locked() != 0) {
                ask_drop_locked();
                snprintf(why, len, "the machine's button cannot be read");
                rc = -1;
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

/* ---- the panel's consent ---- */

static int ended_find_locked(const char *nonce)
{
    for (int i = 0; i < CONSENT_ENDED_MAX; i++)
        if (consent_ended[i].nonce[0] && !strcmp(consent_ended[i].nonce, nonce))
            return i;
    return -1;
}

static void ended_put_locked(const char *nonce, const char *why)
{
    if (ended_find_locked(nonce) >= 0)
        return;
    unsigned i = consent_ended_next++ % CONSENT_ENDED_MAX;
    snprintf(consent_ended[i].nonce, sizeof(consent_ended[i].nonce), "%s", nonce);
    snprintf(consent_ended[i].why, sizeof(consent_ended[i].why), "%s", why);
}

/* The consent's reservation, standing: its slot, or -1. */
static int consent_res_locked(double now)
{
    char who[BTNRES_WHO_MAX];
    snprintf(who, sizeof(who), "panel:%s", consent.nonce);
    return res_find_locked(who, now);
}

/* The consent that stands is over, in @why's words: its nonce is not
 * registered again, and its reservation ends but for the linger after a
 * press. */
static void consent_end_locked(const char *why, double now)
{
    if (consent.state == CONSENT_NONE)
        return;
    int own = consent_res_locked(now);
    double keep = consent.pressed_at > 0 ? consent.pressed_at + consent_linger() : 0;
    if (own >= 0)
        res_tab[own].until = keep > now ? keep : 0;
    ended_put_locked(consent.nonce, why);
    consent.state = CONSENT_NONE;
    consent.pressed_at = 0;
}

/* A consent nobody renews any more, or past its bound, ends. */
static void consent_lapse_locked(double now)
{
    if (consent.state == CONSENT_NONE)
        return;
    if (consent_res_locked(now) < 0)
        consent_end_locked("the dialog stopped asking: start it again from the control panel", now);
    else if (now >= consent.began + BTNRES_CONSENT_S)
        consent_end_locked(consent.state == CONSENT_PRESSED
                               ? "the press was not used in time: start it again from the control panel"
                               : "the machine's button was not pressed in time: start it again from the control panel",
                           now);
}

/* Somebody else's reservation stands beside the consent's own (@own): 1,
 * with whose in @why. A panel reservation beside it is an earlier
 * consent's linger after its press. */
static int consent_others_locked(int own, double now, char *why, size_t len)
{
    for (int i = 0; i < BTNRES_MAX; i++)
        if (i != own && res_tab[i].until > now) {
            if (!strncmp(res_tab[i].who, "panel:", 6))
                snprintf(why, len, "the button was just pressed for another prompt");
            else
                res_words(res_tab[i].who, why, len);
            return 1;
        }
    return 0;
}

/* Whether the press, now, may be the consent's: its reservation on file
 * long enough, no other reservation beside it, and nobody else waiting on
 * the button. 0, or 1 with who in @why. */
static int consent_blocked_locked(int own, double now, char *why, size_t len)
{
    if (!btnres_on_file_s || btnres_on_file_s() < BTNRES_READY_S || now - consent.began < BTNRES_READY_S) {
        snprintf(why, len, "%s", "");
        return 1;
    }
    if (consent_others_locked(own, now, why, len))
        return 1;
    return btnres_consent_refused && btnres_consent_refused(why, len);
}

/* A consent that waits comes in force when nothing blocks it (judged every
 * BTNRES_CONSENT_JUDGE_S at most); one in force that somebody else came to
 * wait beside waits again. A press counts from a release read in force. */
static void consent_judge_locked(double now, int in_force_too)
{
    char why[160];
    if (consent.state == CONSENT_WAIT && now - consent.judged >= BTNRES_CONSENT_JUDGE_S) {
        consent.judged = now;
        if (consent_blocked_locked(consent_res_locked(now), now, why, sizeof(why)))
            snprintf(consent.waits, sizeof(consent.waits), "%s", why);
        else {
            consent.state = CONSENT_READY;
            consent.waits[0] = '\0';
            consent.released = consent.down = 0;
        }
    } else if (consent.state == CONSENT_READY && in_force_too &&
               consent_blocked_locked(consent_res_locked(now), now, why, sizeof(why))) {
        consent.state = CONSENT_WAIT;
        consent.judged = now;
        snprintf(consent.waits, sizeof(consent.waits), "%s", why);
        consent.released = consent.down = 0;
    }
}

int btnres_consent_open(const btnres_consent_t *c, char *why, size_t len, char *other, size_t olen)
{
    char who[BTNRES_WHO_MAX];
    if (other && olen)
        other[0] = '\0';
    snprintf(why, len, "%s", "");
    snprintf(who, sizeof(who), "panel:%s", c->nonce ? c->nonce : "");
    if (!btnres_name_ok(who) || !c->kind || !c->kind[0] || strlen(c->kind) >= BTNRES_KIND_MAX || !c->subject ||
        strlen(c->subject) >= BTNRES_SUBJECT_MAX || !c->holder || !c->holder[0] || strlen(c->holder) >= BTNRES_HOLDER_MAX) {
        snprintf(why, len, "the consent has no usable name");
        return -1;
    }
    double now = res_mono();
    int rc = -1;
    pthread_mutex_lock(&res_mu);
    consent_lapse_locked(now);
    int ended = ended_find_locked(c->nonce);
    if (ended >= 0) {
        snprintf(why, len, "%s", consent_ended[ended].why);
        goto out;
    }
    if (consent.state != CONSENT_NONE && !strcmp(consent.nonce, c->nonce)) {
        /* Its own renewal: the same kind, subject and holder, or it ends. */
        if (strcmp(consent.kind, c->kind) || strcmp(consent.subject, c->subject) || strcmp(consent.holder, c->holder)) {
            consent_end_locked(CONSENT_ELSE, now);
            snprintf(why, len, CONSENT_ELSE);
            goto out;
        }
        int own = consent_res_locked(now);
        double until = now + BTNRES_PANEL_TTL_S, last = consent.began + BTNRES_CONSENT_S;
        if (until > last)
            until = last;
        if (consent.pressed_at > 0 && until < consent.pressed_at + consent_linger())
            until = consent.pressed_at + consent_linger();
        if (own >= 0 && res_tab[own].until < until)
            res_tab[own].until = until;
    } else {
        if (consent.state != CONSENT_NONE) {
            if (strcmp(consent.holder, c->holder)) {
                /* Two consents at once: one press would be one of two
                 * parties'. Both are refused, and the log names both. */
                if (other && olen)
                    snprintf(other, olen, "%s from %s", consent.kind, consent.peer);
                consent_end_locked(CONSENT_OTHER, now);
                ended_put_locked(c->nonce, CONSENT_OTHER);
                snprintf(why, len, CONSENT_OTHER);
                goto out;
            }
            /* The same holder's next dialog: it replaces its own. */
            consent_end_locked("another dialog of this session asked for the press: start it again from the control panel",
                               now);
        }
        if (res_put_locked(who, now, now + BTNRES_PANEL_TTL_S) < 0) {
            snprintf(why, len, "too many prompts reserve the button");
            goto out;
        }
        /* The operator's own prompt: no package's ask stands beside it,
         * but its reservation lingers (ask_drop_lingering_locked). */
        ask_drop_lingering_locked(now);
        memset(&consent, 0, sizeof(consent));
        snprintf(consent.nonce, sizeof(consent.nonce), "%s", c->nonce);
        snprintf(consent.kind, sizeof(consent.kind), "%s", c->kind);
        snprintf(consent.subject, sizeof(consent.subject), "%s", c->subject);
        snprintf(consent.holder, sizeof(consent.holder), "%s", c->holder);
        snprintf(consent.peer, sizeof(consent.peer), "%s", c->peer ? c->peer : "");
        consent.state = CONSENT_WAIT;
        consent.began = now;
        consent.judged = now - BTNRES_CONSENT_JUDGE_S;
        if (reader_start_locked() != 0) {
            consent_end_locked("the machine's button cannot be read", now);
            snprintf(why, len, "the machine's button cannot be read");
            goto out;
        }
    }
    consent_judge_locked(now, 1);
    rc = consent.state == CONSENT_PRESSED ? BTNRES_CONSENT_PRESSED
         : consent.state == CONSENT_READY ? BTNRES_CONSENT_READY : BTNRES_CONSENT_WAITING;
    if (rc == BTNRES_CONSENT_WAITING)
        snprintf(why, len, "%s", consent.waits);
out:
    pthread_mutex_unlock(&res_mu);
    return rc;
}

void btnres_consent_close(const char *nonce)
{
    char who[BTNRES_WHO_MAX];
    if (!nonce)
        return;
    snprintf(who, sizeof(who), "panel:%s", nonce);
    if (!btnres_name_ok(who))
        return;
    double now = res_mono();
    pthread_mutex_lock(&res_mu);
    if (consent.state != CONSENT_NONE && !strcmp(consent.nonce, nonce))
        consent_end_locked("the dialog was closed: start it again from the control panel", now);
    else
        ended_put_locked(nonce, "the dialog was closed: start it again from the control panel");
    pthread_mutex_unlock(&res_mu);
}

int btnres_consent_take(const btnres_consent_t *c, char *why, size_t len)
{
    double now = res_mono();
    int rc;
    pthread_mutex_lock(&res_mu);
    consent_lapse_locked(now);
    int ended = c->nonce ? ended_find_locked(c->nonce) : -1;
    if (consent.state == CONSENT_NONE || !c->nonce || strcmp(consent.nonce, c->nonce)) {
        snprintf(why, len, "%s", ended >= 0 ? consent_ended[ended].why : CONSENT_NONE_WORDS);
        rc = BTNRES_NOT_STANDING;
    } else if (!c->kind || !c->subject || !c->holder || strcmp(consent.kind, c->kind) ||
               strcmp(consent.subject, c->subject) || strcmp(consent.holder, c->holder)) {
        consent_end_locked(CONSENT_ELSE, now);
        snprintf(why, len, CONSENT_ELSE);
        rc = BTNRES_MISMATCH;
    } else if (consent.state != CONSENT_PRESSED) {
        snprintf(why, len, "the machine's button was not pressed for this since the panel asked: press it once, then OK");
        rc = BTNRES_NOT_PRESSED;
    } else {
        consent_end_locked("the press was used: start it again from the control panel", now);
        rc = BTNRES_TAKEN;
    }
    pthread_mutex_unlock(&res_mu);
    return rc;
}

void btnres_consent_step(int button)
{
    char why[160];
    double now = res_mono();
    pthread_mutex_lock(&res_mu);
    consent_lapse_locked(now);
    if (consent.state == CONSENT_WAIT || consent.state == CONSENT_READY) {
        if (button < 0) {
            if (++consent.unread >= BTNRES_ASK_UNREAD_MAX)
                consent_end_locked("the machine's button cannot be read", now);
        } else {
            consent.unread = 0;
            consent_judge_locked(now, 0);
            if (consent.state != CONSENT_READY || button != 1) {
                if (consent.state == CONSENT_READY)
                    consent.released = 1;
                consent.down = 0;
            } else if (consent.released && ++consent.down >= 2) {
                /* The press, read twice in a row: the consent's only if
                 * nothing blocks it now. A press somebody else takes is not
                 * the consent's, which waits again. Either way the
                 * reservation stands WIZPKG_LINGER_S from here, so a second
                 * press made for the dialog arms no job. */
                int own = consent_res_locked(now);
                if (own >= 0 && res_tab[own].until < now + consent_linger())
                    res_tab[own].until = now + consent_linger();
                consent.pressed_at = now;
                if (consent_blocked_locked(own, now, why, sizeof(why))) {
                    consent.state = CONSENT_WAIT;
                    consent.judged = now;
                    snprintf(consent.waits, sizeof(consent.waits), "%s", why);
                    consent.released = consent.down = 0;
                } else
                    consent.state = CONSENT_PRESSED;
            }
        }
    }
    pthread_mutex_unlock(&res_mu);
}
