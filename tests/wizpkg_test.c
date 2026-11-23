/*
 * wizpkg_test.c - host test: a package's check on the Setup page
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The test binary is its own stand-in for the extension host: run as
 * "list" or "wizard ...", it records its arguments and answers from the
 * files the test wrote, and as the host's pid in the status file it is a
 * host that is alive. The runner's calls are stand-ins here too, recording
 * what the check did. Cases: every step held to its form (a log, a phase,
 * a progress, a prompt of each kind, a result, and each way out of form);
 * a check run end to end (start, the prompt asked with the machine's button
 * reserved for the package and kept WIZPKG_LINGER_S after it, a later
 * prompt's wait that ends the check keeping it too, and only once the
 * reservation is on file with no job waiting, the waiting job named first,
 * at once for a package that keeps the machine, never when it does not
 * come on file in time; the answer carried back
 * as the host's answer command, a confirm as yes, the result shown and
 * recorded by nobody); a result that did not pass; the service's refusal;
 * a step out of form, and an operator's abort, each sending abort; a
 * prompt nobody answered, sending abort and leaving the step's reason to
 * the runner, which names the timeout; the host's own refusal; and the
 * Setup page's list, only for a package whose service runs, with its
 * title and whether it is done. A package's ask for a press (btnres.h):
 * refused by name while a job waits or under a dialog's, a check's or
 * another package's reservation; in force only once on file for
 * BTNRES_READY_S, and ended by a job that came to wait before that; the
 * press taken from a release read in force and the button down twice,
 * said once, never one begun earlier or made with the reservation off the
 * file; the linger after it; ended by the operator's prompt (the Setup
 * page's wait leaving its reservation WIZPKG_LINGER_S), by the check of a
 * package that holds the machine, or comes to hold it after the check
 * reserved the button (the same linger, every job withheld,
 * the check's prompt waiting it out), the daemon's
 * own wait, its withdrawal, BTNRES_PANEL_TTL_S with no renewal, and
 * BTNRES_TTL_MAX_S however often it is renewed. The panel's consent: in
 * force only once on file and BTNRES_READY_S old, with no other prompt's
 * reservation beside it and nobody else waiting; its press from a release
 * read in force and two readings down, never one held from before, asked
 * again at the press; taken once; the linger after a press, its own or
 * not; ended by its dialog's close, BTNRES_PANEL_TTL_S with no renewal,
 * BTNRES_CONSENT_S however often renewed, an unreadable button and a
 * renewal for another subject, its nonce never registered again; the same
 * holder's next dialog replacing its own; a package's ask ended and
 * refused while it stands.
 */
#include "../src/btnres.h"
#include "../src/extpkg.h"
#include "../src/wizpkg.h"
#include "../src/wizrun.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/wizpkg-test-XXXXXX";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static const char *read_file(const char *path)
{
    static char text[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    text[n] = '\0';
    return text;
}

/* ---- the host, as this binary run again ---- */

static int stand_in(int argc, char **argv)
{
    const char *d = getenv("WIZPKG_TEST_DIR");
    char path[400];
    snprintf(path, sizeof(path), "%s/args", d);
    FILE *f = fopen(path, "a");
    for (int i = 1; f && i < argc; i++)
        fprintf(f, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
    if (f)
        fclose(f);
    if (!strcmp(argv[1], "list"))
        snprintf(path, sizeof(path), "%s/list.json", d);
    else
        snprintf(path, sizeof(path), "%s/%s.json", d, argc > 3 ? argv[3] : "none");
    const char *text = read_file(path);
    fputs(text[0] ? text : "{\"ok\": false, \"error\": \"no answer written\"}", stdout);
    return strstr(text, "\"ok\": true") ? 0 : 1;
}

/* ---- the runner, as stand-ins that record ---- */

static char logged[1024], phase[128], finished[256], asked[512], answer_given[64];
static int progress = -1, shown, ask_rc;
static const char *current = "pkg:org.example.check";

void wiz_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(logged);
    vsnprintf(logged + n, sizeof(logged) - n, fmt, ap);
    va_end(ap);
    strncat(logged, "|", sizeof(logged) - strlen(logged) - 1);
}
static char phases_seen[512];
void wiz_phase(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(phase, sizeof(phase), fmt, ap);
    va_end(ap);
    strncat(phases_seen, phase, sizeof(phases_seen) - strlen(phases_seen) - 2);
    strncat(phases_seen, "|", sizeof(phases_seen) - strlen(phases_seen) - 1);
}
void wiz_progress(int pct) { progress = pct; }
void wiz_finish_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(finished, sizeof(finished), fmt, ap);
    va_end(ap);
}
void wiz_finish_shown(json_t *result)
{
    char *t = json_dumps(result, JSON_COMPACT | JSON_SORT_KEYS);
    snprintf(finished, sizeof(finished), "shown %s", t ? t : "");
    free(t);
    shown = 1;
}
const char *wiz_current_id(void) { return current; }
/* The operator's Abort, once it has been asked abort_after times (-1: never). */
static int abort_after = -1, abort_asks;
int wiz_aborted(void) { return abort_after >= 0 && ++abort_asks > abort_after; }
static int reserved_at_ask = -1;     /* the button reserved for the package while it asked */
static int ask_lingered_at_ask = -1; /* another package's ask still reserved the button then */
static int outlives_prompt = -1;     /* its reservation still stood past the prompt's whole time */
/* The reservations' clock, which the prompt moves on. */
static double clock_s = 1000.0;
static double fake_clock(void) { return clock_s; }
/* The reservation's time on file, and the job that waits, as the prompt's
 * wait reads them: on file (on_file_value seconds) once the wait has asked
 * on_file_after times, a job waiting for the first waits_for asks. */
static int on_file_asks, on_file_after, waits_asks, waits_for, own_keeps;
static double on_file_at_ask, on_file_value = 1.5;
static double fake_on_file(void) { return ++on_file_asks > on_file_after ? on_file_value : -1.0; }
static int fake_job_waits(char *why, size_t len)
{
    if (++waits_asks > waits_for)
        return 0;
    snprintf(why, len, "a job waits for the button press: cancel it first");
    return 1;
}
/* A job that waits for the press once the check has asked its first prompt. */
static int job_waits_once_asked(char *why, size_t len)
{
    if (!asked[0])
        return 0;
    snprintf(why, len, "a job waits for the button press: cancel it first");
    return 1;
}
/* No job waits, and the reservations' clock moves a second at each ask:
 * the prompt's wait for a linger to end, run in a few real steps. */
static int waits_and_tick(char *why, size_t len)
{
    (void)why;
    (void)len;
    clock_s += 1.0;
    return 0;
}
/* The package keeps the machine once it has been asked own_keeps_after times. */
static int holds_asks, own_keeps_after;
static int fake_pkg_holds(const char *pkg)
{
    return own_keeps && ++holds_asks > own_keeps_after && !strcmp(pkg, "org.example.check");
}
int wiz_ask(const char *kind, const char *pid, const char *text, const char *const *opts, int nopt, char *answer,
            size_t alen)
{
    char why[96];
    on_file_at_ask = on_file_asks > on_file_after ? on_file_value : -1.0;
    reserved_at_ask = btnres_stands("check:org.example.check") && btnres_withheld(why, sizeof(why));
    ask_lingered_at_ask = btnres_any("ask:");
    /* The prompt stands its whole time: the reservation is still there. */
    clock_s += WIZPKG_READY_WAIT_S + WIZ_PROMPT_TIMEOUT_S;
    outlives_prompt = btnres_stands("check:org.example.check");
    snprintf(asked, sizeof(asked), "%s %s %s [", kind, pid, text);
    for (int i = 0; i < nopt; i++) {
        strncat(asked, opts[i], sizeof(asked) - strlen(asked) - 1);
        strncat(asked, i + 1 < nopt ? "," : "", sizeof(asked) - strlen(asked) - 1);
    }
    strncat(asked, "]", sizeof(asked) - strlen(asked) - 1);
    snprintf(answer, alen, "%s", answer_given);
    return ask_rc;
}

static void reset(void)
{
    char path[400];
    logged[0] = phase[0] = finished[0] = asked[0] = phases_seen[0] = '\0';
    progress = -1;
    shown = 0;
    ask_rc = 0;
    on_file_asks = waits_asks = holds_asks = 0;
    snprintf(path, sizeof(path), "%s/args", dir);
    unlink(path);
}

static void answers(const char *verb, const char *text)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/%s.json", dir, verb);
    write_file(path, text);
}

static const char *args(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/args", dir);
    return read_file(path);
}

static int parse(const char *text, wizpkg_step_t *s, char *why, size_t wlen)
{
    json_t *j = json_loads(text, 0, NULL);
    int rc = wizpkg_parse(j, s, why, wlen);
    json_decref(j);
    return rc;
}

/* ---- a package's ask for a press (btnres.h) ---- */

#define ASKER "org.example.check"
#define OTHER "org.example.other"
#define THIRD "org.example.third"
static double ask_on_file = -1.0;       /* the reservation's time on file, as the ask reads it */
static const char *ask_waiter;          /* who else waits on the button, NULL nobody */
static int ask_daemon;                  /* the daemon waits for a press of its own */
static double ask_file(void) { return ask_on_file; }
static int ask_other(const char *pkg, char *why, size_t len)
{
    (void)pkg;
    if (!ask_waiter)
        return 0;
    snprintf(why, len, "%s", ask_waiter);
    return 1;
}
static int ask_daemon_waits(void) { return ask_daemon; }
static int ask_session;                 /* a controller reports its armed session */
static int ask_session_reported(void) { return ask_session; }
static int button_unreadable(void) { return -1; }

/* One call of @pkg's ask: its state, or "409 <who holds the button>". */
static const char *ask(const char *pkg)
{
    static char said[200];
    const char *state = "";
    char why[160] = "";
    if (btnres_ask(pkg, &state, why, sizeof(why)) == 0)
        return state;
    snprintf(said, sizeof(said), "409 %s", why);
    return said;
}

/* The button read @n times at @level, BTNRES_ASK_POLL_MS apart. */
static void button(int level, int n)
{
    for (int i = 0; i < n; i++) {
        clock_s += BTNRES_ASK_POLL_MS / 1000.0;
        btnres_ask_step(level);
    }
}

/* An ask of ASKER in force, the button read released. */
static void ask_in_force(void)
{
    ask_on_file = -1.0;
    ask(ASKER);
    ask_on_file = BTNRES_READY_S;
    button(0, 1);
}

/* The table and the ask emptied between cases. */
static void ask_clear(void)
{
    btnres_ask_end(ASKER);
    btnres_ask_end(OTHER);
    clock_s += BTNRES_TTL_MAX_S + 1.0;
    button(0, 1);
    ask_waiter = NULL;
    ask_daemon = 0;
    ask_session = 0;
    ask_on_file = -1.0;
}

static void ask_cases(void)
{
    char why[160];
    btnres_ask_refused = ask_other;
    btnres_daemon_waits = ask_daemon_waits;
    btnres_session_reported = ask_session_reported;
    btnres_on_file_s = ask_file;

    /* Refused while anybody else waits on the button, by name. */
    ask_waiter = "a job waits for the button press: cancel it first";
    CHECK(!strcmp(ask(ASKER), "409 a job waits for the button press: cancel it first") && !btnres_stands("ask:" ASKER),
          "an ask while a job waits for its press: refused by name, nothing reserved (%s)", ask(ASKER));
    ask_waiter = NULL;
    CHECK(btnres_open("panel:d1", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "a dialog reserves the button");
    CHECK(!strcmp(ask(ASKER), "409 the control panel asks for the button") && !btnres_stands("ask:" ASKER),
          "an ask under the panel's reservation: refused by name (%s)", ask(ASKER));
    btnres_close("panel:d1");
    CHECK(btnres_open("check:" OTHER, BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "a check reserves the button");
    CHECK(!strcmp(ask(ASKER), "409 the check of " OTHER " asks for the button"),
          "an ask under a check's reservation: refused by name (%s)", ask(ASKER));
    btnres_close("check:" OTHER);
    /* The Setup page's reservation: a renewal moves a standing one's end and
     * never opens one that ended. */
    CHECK(btnres_open(BTNRES_SETUP, BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0 && btnres_renew(BTNRES_SETUP, 5.0) == 0 &&
          btnres_stands(BTNRES_SETUP), "the Setup page's reservation is renewed while it stands");
    btnres_close(BTNRES_SETUP);
    CHECK(btnres_renew(BTNRES_SETUP, 5.0) == -1 && !btnres_stands(BTNRES_SETUP),
          "a renewal of the ended reservation opens nothing");

    /* It reserves the button against every job, and is in force only once
     * that has stood on file for BTNRES_READY_S. */
    CHECK(!strcmp(ask(ASKER), "waiting") && btnres_stands("ask:" ASKER) && btnres_withheld(why, sizeof(why)) &&
          !strcmp(why, "the package " ASKER " asks for the button"), "an ask with nobody waiting: reserved, waiting (%s)", why);
    ask_on_file = BTNRES_READY_S - 0.1;
    button(0, 1);
    CHECK(!strcmp(ask(ASKER), "waiting"), "on file for less than BTNRES_READY_S: not in force");
    ask_on_file = BTNRES_READY_S;
    CHECK(!strcmp(ask(ASKER), "ready"), "on file for BTNRES_READY_S: in force");
    /* While it stands, another package's ask is refused by name. */
    CHECK(!strcmp(ask(OTHER), "409 the package " ASKER " asks for the button") && !btnres_stands("ask:" OTHER) &&
          !strcmp(ask(ASKER), "ready"), "another package's ask while it stands: refused by name, the first still in force");
    /* The press: a release read in force, then the button down twice in a
     * row. It is said once, and the reservation stands WIZPKG_LINGER_S. */
    button(0, 1);
    button(1, 1);
    CHECK(!strcmp(ask(ASKER), "ready"), "the button read down once: no press yet");
    button(0, 1);
    button(1, 2);
    CHECK(!strcmp(ask(ASKER), "pressed"), "released, then down twice in a row: the press is the asker's");
    CHECK(!strcmp(ask(OTHER), "409 the package " ASKER " asks for the button"),
          "another package's ask after the press: refused in the reservation that stands (%s)", ask(OTHER));
    CHECK(!strcmp(ask(ASKER), "ready"), "the press is said once: the next call is a new ask (%s)", ask(ASKER));
    ask_clear();
    ask_in_force();
    button(1, 2);
    double pressed_at = clock_s;
    clock_s = pressed_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" ASKER), "the reservation lapsed before WIZPKG_LINGER_S after the press");
    clock_s = pressed_at + WIZPKG_LINGER_S + 0.5;
    CHECK(!btnres_stands("ask:" ASKER), "the reservation stands past WIZPKG_LINGER_S after the press");
    ask_clear();

    /* A press begun before the ask was in force is nobody's. */
    ask(ASKER);
    button(0, 2);
    button(1, 2);
    ask_on_file = BTNRES_READY_S;
    button(1, 4);
    CHECK(!strcmp(ask(ASKER), "ready"), "the button down since before the ask came in force: no press");
    ask_clear();

    /* A job that came to wait before the controllers had read the
     * reservation keeps the button: the ask ends as it would come in force. */
    ask(ASKER);
    ask_waiter = "a job waits for the button press: cancel it first";
    ask_on_file = BTNRES_READY_S;
    button(0, 1);
    CHECK(!btnres_stands("ask:" ASKER), "a job waiting as the ask would come in force: the ask ended, nothing reserved");
    button(1, 3);
    CHECK(!strncmp(ask(ASKER), "409 a job waits", 15), "and the press made then is no package's (%s)", ask(ASKER));
    ask_clear();

    /* A press with the reservation no longer on file is not handed over. */
    ask_in_force();
    ask_on_file = -1.0;
    button(1, 3);
    CHECK(!strcmp(ask(ASKER), "ready"), "a press with the reservation off the file: not the asker's");
    ask_clear();

    /* The operator's own prompt ends an ask that stands, and takes the press. */
    ask_in_force();
    CHECK(btnres_open("panel:d2", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0 && !btnres_stands("ask:" ASKER),
          "a dialog reserving the button ends the ask");
    button(1, 3);
    CHECK(!strcmp(ask(ASKER), "409 the control panel asks for the button"), "and its press is no package's (%s)", ask(ASKER));
    btnres_close("panel:d2");
    ask_clear();

    /* The Setup page's wait ends it too, and refuses the next one; the
     * ask's reservation lingers WIZPKG_LINGER_S, as after a press (the
     * Setup's wait does not come in force beside it, wiz.c). */
    ask_in_force();
    double setup_at = clock_s;
    CHECK(btnres_open(BTNRES_SETUP, BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0 &&
          !strcmp(ask(ASKER), "409 the machine waits for a press of the button for its own setup"),
          "the Setup page's wait ends the ask, and refuses its next one (%s)", ask(ASKER));
    clock_s = setup_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" ASKER), "the ask's reservation lapsed before WIZPKG_LINGER_S after the Setup's wait ended it");
    clock_s = setup_at + WIZPKG_LINGER_S + 0.5;
    CHECK(!btnres_stands("ask:" ASKER), "the ask's reservation stands past WIZPKG_LINGER_S after the Setup's wait ended it");
    btnres_close(BTNRES_SETUP);
    ask_clear();

    /* A package that comes to hold the machine while another package's ask
     * is in force leaves that ask standing; its check then ends the ask,
     * whose reservation lingers WIZPKG_LINGER_S and withholds every job
     * meanwhile (the check withholds nothing from its own package's jobs),
     * and the check's prompt waits the linger out. */
    btnres_pkg_holds = fake_pkg_holds;
    own_keeps = 1;
    own_keeps_after = 0;
    ask_on_file = -1.0;
    ask(OTHER);
    ask_on_file = BTNRES_READY_S;
    button(0, 1);
    ask_waiter = "the package org.example.check holds the machine";
    CHECK(!strcmp(ask(OTHER), "ready"), "an ask in force while another package came to hold the machine: it stands (%s)", ask(OTHER));
    ask_waiter = NULL;
    double check_at = clock_s;
    CHECK(btnres_open("check:org.example.check", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "the check reserves the button");
    clock_s = check_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" OTHER) && btnres_withheld(why, sizeof(why)) && strstr(why, OTHER),
          "the check of a package that holds the machine ended another package's ask with no linger: its own job withheld by nothing");
    btnres_close("check:org.example.check");
    ask_clear();
    /* The same when the check's package comes to hold the machine only
     * after its check reserved the button. */
    own_keeps = 0;
    ask_on_file = -1.0;
    ask(OTHER);
    ask_on_file = BTNRES_READY_S;
    button(0, 1);
    check_at = clock_s;
    CHECK(btnres_open("check:org.example.check", BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "the check reserves the button");
    own_keeps = 1;
    clock_s = check_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" OTHER) && btnres_withheld(why, sizeof(why)) && strstr(why, OTHER),
          "a check whose package came to hold the machine after it reserved the button ended another package's ask with no linger");
    btnres_close("check:org.example.check");
    ask_clear();
    ask_on_file = -1.0;
    ask(OTHER);
    ask_on_file = BTNRES_READY_S;
    button(0, 1);
    wizpkg_on_file_s = fake_on_file;
    wizpkg_job_waits = waits_and_tick;
    reset();
    answers("answer", "{\"ok\": true, \"status\": 200, \"body\": {\"result\": {\"ok\": true, \"summary\": \"done\"}}}");
    ask_lingered_at_ask = -1;
    alarm(10);
    wizpkg_run();
    alarm(0);
    CHECK(shown && ask_lingered_at_ask == 0,
          "the check's prompt asked while another package's ask it ended still lingered (%d, %s)", ask_lingered_at_ask, finished);
    wizpkg_on_file_s = NULL;
    wizpkg_job_waits = NULL;
    btnres_pkg_holds = NULL;
    own_keeps = 0;
    ask_clear();

    /* This daemon coming to wait for a press of its own ends it too. */
    ask_in_force();
    ask_daemon = 1;
    button(0, 1);
    CHECK(!btnres_stands("ask:" ASKER), "the daemon's own wait for a press ends the ask");
    ask_clear();

    /* It ends by itself: withdrawn (or its package turned off or removed),
     * no longer renewed (its package ended, crashed or hangs), and at the
     * bound of one ask however often it is renewed. */
    ask_in_force();
    btnres_ask_end(ASKER);
    CHECK(!btnres_stands("ask:" ASKER), "an ask ended: nothing reserved");
    ask_in_force();
    clock_s += BTNRES_PANEL_TTL_S - 0.5;
    CHECK(btnres_stands("ask:" ASKER), "an ask not renewed lapsed before BTNRES_PANEL_TTL_S");
    clock_s += 1.0;
    button(0, 1);
    button(1, 3);
    CHECK(!btnres_stands("ask:" ASKER), "an ask not renewed stands past BTNRES_PANEL_TTL_S");
    ask_on_file = -1.0;
    CHECK(!strcmp(ask(ASKER), "waiting"), "and a press past it was nobody's");
    ask_clear();
    ask_in_force();
    double began = clock_s;
    while (clock_s < began + BTNRES_TTL_MAX_S - BTNRES_PANEL_TTL_S) {
        clock_s += BTNRES_PANEL_TTL_S / 2;
        ask(ASKER);
    }
    CHECK(btnres_stands("ask:" ASKER), "an ask renewed all along lapsed before BTNRES_TTL_MAX_S");
    clock_s = began + BTNRES_TTL_MAX_S - 1.0;
    ask(ASKER);
    clock_s = began + BTNRES_TTL_MAX_S + 1.0;
    CHECK(!btnres_stands("ask:" ASKER), "an ask renewed all along stands past BTNRES_TTL_MAX_S");
    ask_clear();

    /* Who else waits on the button is asked again at the press: somebody
     * who came since the ask came in force takes the press as their own,
     * so the ask ends there and the press is not said. Its reservation
     * stands WIZPKG_LINGER_S all the same, as after a press that is said. */
    ask_in_force();
    ask_waiter = "a job is running or paused: finish or cancel it first";
    button(1, 2);
    double unsaid_at = clock_s;
    CHECK(!strcmp(ask(ASKER), "409 a job is running or paused: finish or cancel it first"),
          "a job running at the press: the ask ended, the press unsaid, the holder named (%s)", ask(ASKER));
    clock_s = unsaid_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" ASKER), "the reservation of an ask ended at the press lapsed before WIZPKG_LINGER_S");
    clock_s = unsaid_at + WIZPKG_LINGER_S + 0.5;
    CHECK(!btnres_stands("ask:" ASKER), "the reservation of an ask ended at the press stands past WIZPKG_LINGER_S");
    ask_clear();

    /* While a controller reports its armed session, a call keeps a
     * standing ask but extends it no further: it ends BTNRES_PANEL_TTL_S
     * after the last call that renewed it, however often the package
     * calls. */
    ask_in_force();
    double kept_at = clock_s;
    ask_session = 1;
    while (clock_s < kept_at + BTNRES_PANEL_TTL_S - 1.5) {
        clock_s += 1.0;
        ask(ASKER);
    }
    CHECK(btnres_stands("ask:" ASKER), "an ask under a reported armed session lapsed before BTNRES_PANEL_TTL_S from its last renewal");
    clock_s = kept_at + BTNRES_PANEL_TTL_S + 0.5;
    CHECK(!btnres_stands("ask:" ASKER), "an ask kept calling under a reported armed session stands past BTNRES_PANEL_TTL_S");
    ask_clear();
    /* And such a call is told "waiting", from the first one: a press made
     * now would not be said to the package. "ready" again once the
     * session is over. */
    ask_in_force();
    ask_session = 1;
    CHECK(!strcmp(ask(ASKER), "waiting"), "a call that keeps an ask in force under a reported armed session: told ready");
    ask_session = 0;
    CHECK(!strcmp(ask(ASKER), "ready"), "the session over: the ask in force is not told ready again");
    ask_clear();

    /* The press is two readings down in a row, counted from a release
     * that was read: down, released, down is none, and an unreadable
     * reading is no release. */
    ask_in_force();
    button(1, 1);
    button(0, 1);
    button(1, 1);
    CHECK(!strcmp(ask(ASKER), "ready"), "down, released, down: no press, the two readings were not in a row");
    ask_clear();
    ask(ASKER);
    button(1, 2);
    ask_on_file = BTNRES_READY_S;
    button(1, 1);
    button(-1, 1);
    button(1, 2);
    CHECK(!strcmp(ask(ASKER), "ready"), "held since before it came in force, one reading unreadable: no release, no press");
    ask_clear();

    /* The ask is its package's own: another package's call leaves it as it
     * is, a press not yet said included, and ends only its own. */
    ask_in_force();
    button(1, 2);
    ask(OTHER);
    CHECK(!strcmp(ask(ASKER), "pressed"), "another package's call before the press was said: the press still the asker's");
    ask_clear();
    ask_in_force();
    btnres_ask_end(OTHER);
    CHECK(btnres_stands("ask:" ASKER) && !strcmp(ask(ASKER), "ready"), "another package ending its ask: the asker's stands");
    ask_clear();

    /* The reservation after a press stands its WIZPKG_LINGER_S whatever
     * ends the ask then, and a press not yet said stays the asker's over
     * readings that cannot be read. */
    ask_in_force();
    button(1, 2);
    btnres_ask_end(ASKER);
    CHECK(btnres_stands("ask:" ASKER), "the ask ended after its press: the reservation stands its linger");
    ask_clear();
    ask_in_force();
    button(1, 2);
    button(-1, BTNRES_ASK_UNREAD_MAX);
    CHECK(!strcmp(ask(ASKER), "pressed"), "the button unreadable after the press was read: the press still said");
    ask_clear();

    /* A button that cannot be read: BTNRES_ASK_UNREAD_MAX readings in a
     * row end the ask, and a new one is refused in those words. */
    ask_in_force();
    button(-1, BTNRES_ASK_UNREAD_MAX - 1);
    button(0, 1);
    button(-1, BTNRES_ASK_UNREAD_MAX - 1);
    CHECK(btnres_stands("ask:" ASKER), "the button unreadable, read, and unreadable again, never BTNRES_ASK_UNREAD_MAX in a row: the ask stands");
    button(-1, 1);
    CHECK(!btnres_stands("ask:" ASKER), "the button unreadable for BTNRES_ASK_UNREAD_MAX readings in a row: the ask ended");
    btnres_button = button_unreadable;
    CHECK(!strcmp(ask(ASKER), "409 the machine's button cannot be read") && !btnres_stands("ask:" ASKER),
          "an ask over a button that cannot be read: refused in those words (%s)", ask(ASKER));
    btnres_button = NULL;
    ask_clear();

    /* A package turned off on the control panel: its ask ends, and every
     * ask of its own is refused until it is turned on again; another
     * package's is not. */
    ask_in_force();
    ask_on_file = -1.0;
    btnres_ask_bar(ASKER);
    CHECK(!btnres_stands("ask:" ASKER), "the asker turned off: its ask ended");
    CHECK(!strncmp(ask(ASKER), "409 the package " ASKER " was turned off", 47) && !btnres_stands("ask:" ASKER),
          "the turned-off package asks again: refused, nothing reserved (%s)", ask(ASKER));
    CHECK(!strcmp(ask(OTHER), "waiting"), "another package's ask beside it: taken (%s)", ask(OTHER));
    btnres_ask_end(OTHER);
    btnres_ask_unbar(ASKER);
    CHECK(!strcmp(ask(ASKER), "waiting"), "turned on again: its ask is taken (%s)", ask(ASKER));
    ask_clear();
    /* The bar is each package's own: turning others off leaves the
     * asker's ask alone, and turning one on lifts no other's bar. */
    ask_in_force();
    btnres_ask_bar(OTHER);
    btnres_ask_bar(THIRD);
    CHECK(btnres_stands("ask:" ASKER) && !strcmp(ask(ASKER), "ready"), "other packages turned off: the asker's ask stands");
    btnres_ask_end(ASKER);
    btnres_ask_unbar(OTHER);
    CHECK(!strncmp(ask(THIRD), "409 the package " THIRD " was turned off", 47),
          "one package turned on again: another's stays refused (%s)", ask(THIRD));
    btnres_ask_unbar(THIRD);
    ask_clear();
    /* From the operator's click the bar is pending: the ask ends and the
     * package is refused in words that say it is being turned off. The
     * host's yes makes it stand, and a later click on a package already
     * turned off makes no new entry for its caller to lift. */
    ask_in_force();
    ask_on_file = -1.0;
    CHECK(btnres_ask_bar_pending(ASKER) == 1 && !btnres_stands("ask:" ASKER), "the click: no new entry, or the ask left standing");
    CHECK(strstr(ask(ASKER), " is being turned off") != NULL && !btnres_stands("ask:" ASKER),
          "asked while the host is asked: not refused as being turned off (%s)", ask(ASKER));
    btnres_ask_bar(ASKER);
    CHECK(strstr(ask(ASKER), " was turned off") != NULL, "the host's yes: the bar does not stand (%s)", ask(ASKER));
    CHECK(btnres_ask_bar_pending(ASKER) == 0 && strstr(ask(ASKER), " was turned off") != NULL,
          "a click on a package already turned off: a new entry, or the bar made pending again (%s)", ask(ASKER));
    btnres_ask_unbar(ASKER);
    ask_clear();

    btnres_ask_refused = NULL;
    btnres_daemon_waits = NULL;
    btnres_session_reported = NULL;
    btnres_on_file_s = NULL;
}

/* A wait with no bound fails the test rather than hanging it. */
static void on_hang(int sig)
{
    static const char m[] = "FAIL: the reservation's wait never ended\n";
    (void)sig;
    if (write(1, m, sizeof(m) - 1) < 0) { /* nothing more to say */ }
    _exit(1);
}

/* ---- the panel's consent (btnres.h) ---- */

static const char *consent_waiter;      /* who else waits on the button, NULL nobody */
static int consent_other(char *why, size_t len)
{
    if (!consent_waiter)
        return 0;
    snprintf(why, len, "%s", consent_waiter);
    return 1;
}

/* One registration or renewal of @c: its state, or "409 <why>". */
static const char *consent(const btnres_consent_t *c)
{
    static char said[240];
    char why[200], other[120];
    int st = btnres_consent_open(c, why, sizeof(why), other, sizeof(other));
    if (st < 0) {
        snprintf(said, sizeof(said), "409 %s", why);
        return said;
    }
    return st == BTNRES_CONSENT_PRESSED ? "pressed" : st == BTNRES_CONSENT_READY ? "ready" : "waiting";
}

/* The button read @n times at @level for the consent, BTNRES_ASK_POLL_MS apart. */
static void creads(int level, int n)
{
    for (int i = 0; i < n; i++) {
        clock_s += BTNRES_ASK_POLL_MS / 1000.0;
        btnres_consent_step(level);
    }
}

/* @c registered, renewed in force, and the button read released. */
static void consent_in_force(const btnres_consent_t *c)
{
    ask_on_file = BTNRES_READY_S;
    consent(c);
    clock_s += BTNRES_READY_S + BTNRES_CONSENT_JUDGE_S;
    consent(c);
    creads(0, 1);
}

/* Every consent and reservation lapsed between cases. */
static void consent_clear(void)
{
    clock_s += BTNRES_CONSENT_S + 1.0;
    creads(0, 1);
    consent_waiter = NULL;
    ask_on_file = -1.0;
}

static void consent_cases(void)
{
    char why[200];
    btnres_consent_refused = consent_other;
    btnres_on_file_s = ask_file;
    btnres_consent_t c = { "c1", "reset", "", "s:one", "192.0.2.1" };

    /* Registered: the button reserved against every job, and not yet in
     * force. A press begun before it is in force is nobody's, and one held
     * down as it comes in force must be released first. */
    ask_on_file = -1.0;
    CHECK(!strcmp(consent(&c), "waiting") && btnres_stands("panel:c1") && btnres_withheld(why, sizeof(why)),
          "a consent registered: reserved against every job, waiting");
    creads(1, 3);
    ask_on_file = BTNRES_READY_S;
    clock_s += BTNRES_READY_S + BTNRES_CONSENT_JUDGE_S;
    CHECK(!strcmp(consent(&c), "ready"), "in force once on file and BTNRES_READY_S old: %s", consent(&c));
    creads(1, 3);
    CHECK(!strcmp(consent(&c), "ready"), "a press held from before it came in force is not its press: %s", consent(&c));
    /* The press: a release read in force, then down twice in a row. */
    creads(0, 1);
    creads(1, 1);
    creads(0, 1);
    CHECK(!strcmp(consent(&c), "ready"), "one reading down is no press: %s", consent(&c));
    creads(1, 2);
    double pressed_at = clock_s;
    CHECK(!strcmp(consent(&c), "pressed"), "released in force, then down twice: its press (%s)", consent(&c));
    /* Taken once, and the reservation stands WIZPKG_LINGER_S after the
     * press, its dialog closed or not. Its nonce is not registered again. */
    CHECK(btnres_consent_take(&c, why, sizeof(why)) == BTNRES_TAKEN &&
          btnres_consent_take(&c, why, sizeof(why)) == BTNRES_NOT_STANDING, "the press taken once (%s)", why);
    btnres_consent_close("c1");
    clock_s = pressed_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("panel:c1"), "the press's reservation lapsed before WIZPKG_LINGER_S");
    clock_s = pressed_at + WIZPKG_LINGER_S + 0.5;
    CHECK(!btnres_stands("panel:c1"), "the press's reservation stands past WIZPKG_LINGER_S");
    CHECK(!strncmp(consent(&c), "409 ", 4) && !btnres_stands("panel:c1"), "an ended consent's nonce registers nothing again");
    consent_clear();

    /* Closed with no press: its reservation ends at once. */
    c.nonce = "c2";
    consent_in_force(&c);
    btnres_consent_close("c2");
    CHECK(!btnres_stands("panel:c2"), "a consent closed with no press: its reservation ended");
    consent_clear();

    /* Another prompt's reservation beside it (a package's check), or
     * somebody else waiting on the button: not in force, and a press then
     * is not its own. Asked again at the press: somebody who came to wait
     * once it was in force takes the press, and the reservation stands
     * WIZPKG_LINGER_S after it. */
    c.nonce = "c3";
    CHECK(btnres_open("check:" OTHER, BTNRES_PANEL_TTL_S, why, sizeof(why)) == 0, "a check reserves the button");
    consent_in_force(&c);
    CHECK(!strcmp(consent(&c), "waiting"), "a check's reservation beside it: not in force (%s)", consent(&c));
    creads(1, 2);
    creads(0, 1);
    creads(1, 2);
    CHECK(!strcmp(consent(&c), "waiting"), "and a press then is not its own: %s", consent(&c));
    btnres_close("check:" OTHER);
    clock_s += BTNRES_CONSENT_JUDGE_S;
    CHECK(!strcmp(consent(&c), "ready"), "the check gone: in force (%s)", consent(&c));
    creads(0, 1);
    consent_waiter = "a job waits for the button press: cancel it first";
    creads(1, 2);
    double unsaid_at = clock_s;
    consent_waiter = NULL;
    CHECK(btnres_consent_take(&c, why, sizeof(why)) == BTNRES_NOT_PRESSED,
          "a job come to wait at the press: the press is not the consent's (%s)", why);
    btnres_consent_close("c3");
    clock_s = unsaid_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("panel:c3"), "the reservation after a press not its own lapsed before WIZPKG_LINGER_S");
    consent_clear();
    c.nonce = "c4";
    consent_waiter = "the machine waits for a press of the button for its own setup";
    consent_in_force(&c);
    CHECK(!strcmp(consent(&c), "waiting"), "somebody else waiting: not in force (%s)", consent(&c));
    consent_clear();

    /* Not renewed: it ends BTNRES_PANEL_TTL_S after its last renewal. */
    c.nonce = "c5";
    consent_in_force(&c);
    clock_s += BTNRES_PANEL_TTL_S + 0.5;
    creads(0, 1);
    CHECK(!btnres_stands("panel:c5") && !strncmp(consent(&c), "409 the dialog stopped asking", 29),
          "a consent not renewed: ended (%s)", consent(&c));
    consent_clear();

    /* Renewed all along, it ends BTNRES_CONSENT_S after its registration. */
    c.nonce = "c6";
    double began = clock_s;
    consent_in_force(&c);
    while (clock_s < began + BTNRES_CONSENT_S - 2.0) {
        clock_s += 2.0;
        consent(&c);
    }
    CHECK(btnres_stands("panel:c6"), "a consent renewed all along lapsed before BTNRES_CONSENT_S");
    clock_s = began + BTNRES_CONSENT_S + 0.1;
    CHECK(!strncmp(consent(&c), "409 ", 4) && !btnres_stands("panel:c6"),
          "a consent renewed all along stands past BTNRES_CONSENT_S: %s", consent(&c));
    consent_clear();

    /* Pressed near BTNRES_CONSENT_S, with no renewal after the press: the
     * press and its use come within BTNRES_CONSENT_S, so a take past it is
     * refused; and the reservation stands WIZPKG_LINGER_S after the press,
     * past the cap the renewals kept to. */
    c.nonce = "c6b";
    began = clock_s;
    consent_in_force(&c);
    while (clock_s < began + BTNRES_CONSENT_S - 2.0) {
        clock_s += 2.0;
        consent(&c);
    }
    clock_s = began + BTNRES_CONSENT_S - 0.6;
    creads(0, 1);
    creads(1, 2);
    double pressed_late = clock_s;
    clock_s = pressed_late + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("panel:c6b"), "a press near BTNRES_CONSENT_S: its reservation lapsed before WIZPKG_LINGER_S");
    CHECK(btnres_consent_take(&c, why, sizeof(why)) != BTNRES_TAKEN, "a press taken past BTNRES_CONSENT_S");
    consent_clear();

    /* The button unreadable BTNRES_ASK_UNREAD_MAX readings in a row: it ends. */
    c.nonce = "c7";
    consent_in_force(&c);
    creads(-1, BTNRES_ASK_UNREAD_MAX);
    CHECK(!btnres_stands("panel:c7") && strstr(consent(&c), "cannot be read"), "the button unreadable: the consent ended");
    consent_clear();

    /* A take before the press leaves it standing; a renewal for another
     * subject ends it; the same holder's next dialog replaces its own. */
    c.nonce = "c8";
    consent_in_force(&c);
    CHECK(btnres_consent_take(&c, why, sizeof(why)) == BTNRES_NOT_PRESSED && !strcmp(consent(&c), "ready"),
          "a take before its press: refused, the consent stands");
    btnres_consent_t d = c;
    d.subject = "something else";
    CHECK(!strncmp(consent(&d), "409 the press was asked for something else", 42) && !btnres_stands("panel:c8"),
          "a renewal for another subject: the consent ended");
    c.nonce = "c9";
    consent(&c);
    btnres_consent_t e = c;
    e.nonce = "c10";
    e.kind = "fuse";
    CHECK(!strcmp(consent(&e), "waiting") && !btnres_stands("panel:c9") && btnres_stands("panel:c10"),
          "the same holder's next dialog replaces its own");
    consent_clear();

    /* The operator's consent ends a package's ask that stands, and a
     * package's ask is refused while it stands. The ask's reservation
     * lingers WIZPKG_LINGER_S: the consent is not in force meanwhile, so a
     * press made for the package's prompt before its page learned of the
     * end is not the consent's; after the linger the consent comes in
     * force. */
    ask_waiter = NULL;
    ask_on_file = -1.0;
    CHECK(!strcmp(ask(ASKER), "waiting"), "a package asks");
    c.nonce = "c11";
    double dropped_at = clock_s;
    consent(&c);
    CHECK(!strcmp(ask(ASKER), "409 the control panel asks for the button"),
          "a consent ends the package's ask, and refuses its next one (%s)", ask(ASKER));
    ask_on_file = BTNRES_READY_S;
    clock_s += BTNRES_READY_S + BTNRES_CONSENT_JUDGE_S;
    CHECK(btnres_stands("ask:" ASKER) && !strcmp(consent(&c), "waiting"),
          "the ask's reservation lingers, and the consent is not in force beside it (%s)", consent(&c));
    creads(0, 1);
    creads(1, 2);
    CHECK(!strcmp(consent(&c), "waiting"), "a press made for the package's prompt is not the consent's: %s", consent(&c));
    clock_s = dropped_at + WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("ask:" ASKER) && !strcmp(consent(&c), "waiting"),
          "the ask's reservation lapsed before WIZPKG_LINGER_S, or the consent came in force beside it (%s)", consent(&c));
    creads(0, 1);
    creads(1, 2);
    CHECK(!strcmp(consent(&c), "waiting"), "a press late in the ask's linger is the consent's: %s", consent(&c));
    creads(0, 1);
    clock_s = dropped_at + WIZPKG_LINGER_S + 0.1;
    CHECK(!strcmp(consent(&c), "ready"), "the ask's linger over: the consent in force (%s)", consent(&c));
    consent_clear();

    /* An earlier consent's press keeps the next consent out for its
     * linger: a double press made for the first dialog is never the
     * second's. */
    c.nonce = "c12";
    consent_in_force(&c);
    creads(1, 2);
    CHECK(btnres_consent_take(&c, why, sizeof(why)) == BTNRES_TAKEN, "the first dialog's press taken (%s)", why);
    btnres_consent_close("c12");
    double taken_at = clock_s;
    btnres_consent_t n2 = c;
    n2.nonce = "c13";
    consent(&n2);
    clock_s += BTNRES_READY_S + BTNRES_CONSENT_JUDGE_S;
    CHECK(!strcmp(consent(&n2), "waiting"), "an earlier consent's linger beside it: not in force (%s)", consent(&n2));
    creads(0, 1);
    creads(1, 2);
    CHECK(!strcmp(consent(&n2), "waiting"), "a second press inside that linger is not the next consent's: %s", consent(&n2));
    clock_s = taken_at + WIZPKG_LINGER_S + 0.1;
    CHECK(!strcmp(consent(&n2), "ready"), "the linger over: the next consent in force (%s)", consent(&n2));
    consent_clear();

    /* In force, then somebody comes to wait on the button: the next
     * renewal answers waiting, so the dialog stops asking for the press. */
    c.nonce = "c14";
    consent_in_force(&c);
    CHECK(!strcmp(consent(&c), "ready"), "in force (%s)", consent(&c));
    consent_waiter = "a job waits for the button press: cancel it first";
    clock_s += BTNRES_CONSENT_JUDGE_S;
    CHECK(!strcmp(consent(&c), "waiting"), "somebody waits: the renewal answers waiting (%s)", consent(&c));
    consent_clear();

    /* Pressed, then not renewed for BTNRES_PANEL_TTL_S (a tab the browser
     * throttles): the consent lapsed, and a take finds no consent. */
    c.nonce = "c15";
    consent_in_force(&c);
    creads(1, 2);
    CHECK(!strcmp(consent(&c), "pressed"), "pressed (%s)", consent(&c));
    clock_s += BTNRES_PANEL_TTL_S + 0.5;
    CHECK(btnres_consent_take(&c, why, sizeof(why)) == BTNRES_NOT_STANDING,
          "a take after the consent lapsed unrenewed takes nothing (%s)", why);
    consent_clear();

    /* The take holds every field: the same name with another subject, or
     * another holder, is refused and ends the consent. */
    btnres_consent_t t = c;
    c.nonce = "c16";
    t = c;
    t.subject = "another archive";
    consent_in_force(&c);
    creads(1, 2);
    CHECK(btnres_consent_take(&t, why, sizeof(why)) == BTNRES_MISMATCH && !strncmp(consent(&c), "409 ", 4),
          "a take naming another subject: refused, the consent ended (%s)", why);
    consent_clear();
    c.nonce = "c17";
    t = c;
    t.holder = "s:two";
    consent_in_force(&c);
    creads(1, 2);
    CHECK(btnres_consent_take(&t, why, sizeof(why)) == BTNRES_MISMATCH && !strncmp(consent(&c), "409 ", 4),
          "a take by another holder: refused, the consent ended (%s)", why);
    consent_clear();

    /* A renewal holds every field too: one with another kind, or by
     * another holder, under the same name ends the consent. */
    c.nonce = "c18";
    t = c;
    t.kind = "fuse";
    consent_in_force(&c);
    CHECK(!strncmp(consent(&t), "409 ", 4) && !btnres_stands("panel:c18"),
          "a renewal for another kind: the consent ended (%s)", consent(&t));
    consent_clear();
    c.nonce = "c19";
    t = c;
    t.holder = "s:two";
    consent_in_force(&c);
    CHECK(!strncmp(consent(&t), "409 ", 4) && !btnres_stands("panel:c19"),
          "a renewal by another holder: the consent ended (%s)", consent(&t));
    consent_clear();
    btnres_consent_refused = NULL;
    btnres_on_file_s = NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "list") || !strcmp(argv[1], "wizard")))
        return stand_in(argc, argv);
    if (!mkdtemp(dir))
        return 1;
    btnres_clock = fake_clock;
    char self[PATH_MAX], path[400], text[2048], why[300];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    self[n > 0 ? n : 0] = '\0';
    setenv("FORGECTRL_FORGEEXT", self, 1);
    setenv("WIZPKG_TEST_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/status.json", dir);
    setenv("FORGECTRL_EXT_STATUS", path, 1);
    wizpkg_step_t s;

    /* Each step held to its form. */
    CHECK(parse("{\"log\": [\"a\", \"b\"], \"phase\": \"looking\", \"progress\": 40, \"prompt\": {\"kind\": \"choice\", "
                "\"id\": \"fan-1\", \"text\": \"Which fan?\", \"options\": [\"left\", \"right\"]}}", &s, why, sizeof(why)) == 0
          && s.nlog == 2 && s.progress == 40 && !strcmp(s.phase, "looking") && s.has_prompt && s.nopt == 2
          && !strcmp(s.opts[1], "right"), "a full step: %s", why);
    CHECK(parse("{\"prompt\": {\"kind\": \"confirm\", \"id\": \"q\", \"text\": \"Sure?\"}}", &s, why, sizeof(why)) == 0
          && s.nopt == 2 && !strcmp(s.opts[0], "Yes") && !strcmp(s.opts[1], "No"), "a confirm gets Yes and No: %s", why);
    CHECK(parse("{\"prompt\": {\"kind\": \"number\", \"id\": \"q\", \"text\": \"How many?\"}}", &s, why, sizeof(why)) == 0
          && s.nopt == 1 && !strcmp(s.opts[0], "OK"), "a number gets OK: %s", why);
    CHECK(parse("{\"result\": {\"ok\": true, \"summary\": \"fine\"}}", &s, why, sizeof(why)) == 0 && s.has_result && s.ok
          && !strcmp(s.summary, "fine"), "a result: %s", why);
    static const char *const bad[] = {
        "[1]", "{}", "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\", \"text\": \"t\"}, \"result\": {\"ok\": true}}",
        "{\"prompt\": {\"kind\": \"jog\", \"id\": \"a\", \"text\": \"t\"}}",
        "{\"prompt\": {\"kind\": \"multichoice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"x\", \"y\"]}}",
        "{\"prompt\": {\"kind\": \"continue\", \"id\": \"A b\", \"text\": \"t\"}}",
        "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\"}}",
        "{\"prompt\": {\"kind\": \"choice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"only\"]}}",
        "{\"prompt\": {\"kind\": \"choice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"\", \"y\"]}}",
        "{\"result\": {\"summary\": \"no ok\"}}", "{\"result\": {\"ok\": \"yes\"}}",
        "{\"progress\": 101, \"result\": {\"ok\": true}}", "{\"progress\": 1.5, \"result\": {\"ok\": true}}",
        "{\"log\": \"one line\", \"result\": {\"ok\": true}}", "{\"log\": [1], \"result\": {\"ok\": true}}",
        "{\"log\": [\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"], \"result\": {\"ok\": true}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = '\0';
        CHECK(parse(bad[i], &s, why, sizeof(why)) != 0 && why[0], "out of form taken: %s", bad[i]);
    }
    snprintf(text, sizeof(text), "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\", \"text\": \"%0400d\"}}", 0);
    CHECK(parse(text, &s, why, sizeof(why)) != 0, "a prompt's text past 399 bytes taken");

    /* A check run end to end. */
    reset();
    answers("start", "{\"ok\": true, \"id\": \"org.example.check\", \"status\": 200, \"body\": {\"log\": [\"looking\"], "
                     "\"progress\": 50, \"prompt\": {\"kind\": \"confirm\", \"id\": \"fan\", \"text\": \"Is it on?\"}}}");
    answers("answer", "{\"ok\": true, \"id\": \"org.example.check\", \"status\": 200, \"body\": {\"log\": [\"thanks\"], "
                      "\"result\": {\"ok\": true, \"summary\": \"the exhaust runs\"}}}");
    snprintf(answer_given, sizeof(answer_given), "Yes");
    wizpkg_run();
    CHECK(shown && !strcmp(finished, "shown {\"ok\":true,\"summary\":\"the exhaust runs\"}"), "the result: %s", finished);
    CHECK(!strcmp(asked, "confirm fan Is it on? [Yes,No]"), "the prompt asked: %s", asked);
    /* The prompt's words may ask for the machine's button: it is reserved
     * against every other party's job while the prompt stands, and for
     * WIZPKG_LINGER_S once it is answered, while the Setup page may still
     * show the question; no longer. */
    CHECK(reserved_at_ask == 1 && btnres_stands("check:org.example.check"),
          "the button reserved for the prompt (%d) and still once it is answered", reserved_at_ask);
    clock_s += WIZPKG_LINGER_S - 0.5;
    CHECK(btnres_stands("check:org.example.check"), "the reservation lapsed before WIZPKG_LINGER_S after the prompt");
    clock_s += 1.0;
    CHECK(!btnres_stands("check:org.example.check"), "the reservation stands past WIZPKG_LINGER_S after the prompt");
    CHECK(outlives_prompt == 1, "the reservation outlives the prompt's wait and its whole time");
    CHECK(!strcmp(logged, "looking|thanks|") && progress == 50, "the log and the progress: %s %d", logged, progress);
    CHECK(!strcmp(args(), "wizard org.example.check start\nwizard org.example.check answer {\"id\":\"fan\",\"answer\":\"yes\"}\n"),
          "the host was asked: [%s]", args());

    /* The prompt waits until its reservation has stood on file for
     * BTNRES_READY_S with no job waiting for the press, and names the
     * waiting job meanwhile; a package that keeps the machine waits for
     * nothing; a reservation that never comes on file ends the check
     * unasked, the host told abort. */
    wizpkg_on_file_s = fake_on_file;
    wizpkg_job_waits = fake_job_waits;
    btnres_pkg_holds = fake_pkg_holds;
    reset();
    on_file_after = 2;
    waits_for = 0;
    wizpkg_run();
    CHECK(shown && on_file_at_ask >= BTNRES_READY_S, "asked only once the reservation is on file (%.1f)", on_file_at_ask);
    reset();
    on_file_after = 0;
    waits_for = 2;
    wizpkg_run();
    CHECK(shown && waits_asks > 2 && strstr(phases_seen, "a job waits for the button press"),
          "asked only once no job waits, the waiting job named first: %s", phases_seen);
    reset();
    on_file_after = 1000000;
    waits_for = 0;
    own_keeps = 1;
    wizpkg_run();
    CHECK(shown && on_file_at_ask < 0 && reserved_at_ask == 0,
          "a package that keeps the machine: asked at once, its reservation withholding nothing from its own job");
    reset();
    own_keeps_after = 3;
    wizpkg_run();
    CHECK(shown && holds_asks > 3, "a package that comes to keep the machine in the wait: asked then");
    own_keeps_after = 0;
    reset();
    own_keeps = 0;
    signal(SIGALRM, on_hang);
    alarm(10);
    wizpkg_run();
    alarm(0);
    CHECK(!shown && !asked[0] && finished[0] && strstr(args(), "abort") && !btnres_stands("check:org.example.check"),
          "never on file: the check ends unasked, released, the host told abort: %s", finished);
    /* On file, but not for the second both controllers need. */
    reset();
    on_file_after = 0;
    on_file_value = 0.5;
    alarm(10);
    wizpkg_run();
    alarm(0);
    CHECK(!shown && !asked[0] && strstr(args(), "abort"),
          "on file for less than its second: never asked: %s", finished);
    on_file_value = 1.5;
    /* The operator's Abort in the wait ends the check there, unasked. */
    reset();
    on_file_after = 1000000;
    abort_after = 3;
    abort_asks = 0;
    alarm(10);
    wizpkg_run();
    alarm(0);
    CHECK(!shown && !asked[0] && !strcmp(finished, "aborted") && strstr(args(), "abort") &&
          !btnres_stands("check:org.example.check"),
          "an Abort in the wait ends the check unasked, released, the host told abort: %s", finished);
    /* A first prompt answered, and the next prompt's wait ended by an Abort
     * while a job waits for the press: the first question may still be on
     * a Setup page, so the reservation keeps WIZPKG_LINGER_S. */
    wizpkg_job_waits = job_waits_once_asked;
    reset();
    on_file_after = 0;
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"continue\", \"id\": \"one\", "
                     "\"text\": \"Press the machine's button, then Continue\"}}}");
    answers("answer", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"continue\", \"id\": \"two\", "
                      "\"text\": \"And again\"}}}");
    snprintf(answer_given, sizeof(answer_given), "Continue");
    abort_after = 0;
    abort_asks = 0;
    alarm(10);
    wizpkg_run();
    alarm(0);
    CHECK(asked[0] && !strcmp(finished, "aborted") && btnres_stands("check:org.example.check"),
          "a later prompt's wait that ends the check keeps the reservation: asked [%s], %s", asked, finished);
    clock_s += WIZPKG_LINGER_S + 1.0;
    CHECK(!btnres_stands("check:org.example.check"), "the reservation stands past WIZPKG_LINGER_S after the wait");
    answers("start", "{\"ok\": true, \"id\": \"org.example.check\", \"status\": 200, \"body\": {\"log\": [\"looking\"], "
                     "\"progress\": 50, \"prompt\": {\"kind\": \"confirm\", \"id\": \"fan\", \"text\": \"Is it on?\"}}}");
    snprintf(answer_given, sizeof(answer_given), "Yes");
    abort_after = -1;
    wizpkg_on_file_s = NULL;
    wizpkg_job_waits = NULL;
    btnres_pkg_holds = NULL;

    ask_cases();
    consent_cases();

    /* A result that did not pass. */
    reset();
    answers("answer", "{\"ok\": true, \"status\": 200, \"body\": {\"result\": {\"ok\": false, \"summary\": \"no exhaust\"}}}");
    wizpkg_run();
    CHECK(!shown && !strcmp(finished, "no exhaust"), "a check that did not pass: %s", finished);

    /* The service's refusal, a step out of form, and an abort: each sends abort. */
    answers("abort", "{\"ok\": true, \"status\": 200, \"body\": {}}");
    reset();
    answers("start", "{\"ok\": true, \"status\": 409, \"body\": {\"error\": \"the fan is not mine\"}}");
    wizpkg_run();
    CHECK(!strcmp(finished, "the check answered 409: the fan is not mine") && strstr(args(), "wizard org.example.check abort"),
          "the service's refusal: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"jog\", \"id\": \"x\", \"text\": \"t\"}}}");
    wizpkg_run();
    CHECK(strstr(finished, "kind is continue, confirm, number, or choice") && strstr(args(), "abort"),
          "a step out of form: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"continue\", \"id\": \"x\", \"text\": \"t\"}}}");
    ask_rc = -1;
    wizpkg_run();
    CHECK(!strcmp(finished, "aborted") && strstr(args(), "abort"), "an operator's abort: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"continue\", \"id\": \"x\", \"text\": \"t\"}}}");
    ask_rc = -2;
    wizpkg_run();
    CHECK(!finished[0] && strstr(args(), "abort"),
          "a prompt nobody answered: no reason over the runner's, and the check told to abort: '%s' [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": false, \"error\": \"its service is frozen while a job is armed\"}");
    wizpkg_run();
    CHECK(!strcmp(finished, "its service is frozen while a job is armed"), "the host's refusal: %s", finished);

    /* The Setup page's list: a host that is alive, a service that runs. */
    snprintf(path, sizeof(path), "%s/list.json", dir);
    write_file(path, "{\"ok\": true, \"packages\": ["
               "{\"id\": \"org.example.check\", \"enabled\": true, \"effective\": [\"wizard\"], \"package\": {\"name\": \"Check\"}},"
               "{\"id\": \"org.example.off\", \"enabled\": false, \"effective\": [\"wizard\"], \"package\": {\"name\": \"Off\"}},"
               "{\"id\": \"org.example.plain\", \"enabled\": true, \"effective\": [\"events\"], \"package\": {\"name\": \"Plain\"}},"
               "{\"id\": \"org.example.stopped\", \"enabled\": true, \"effective\": [\"wizard\"], \"package\": {\"name\": \"S\"}}]}");
    snprintf(text, sizeof(text), "{\"pid\": %d, \"services\": [{\"id\": \"org.example.check\", \"state\": \"running\"}, "
             "{\"id\": \"org.example.off\", \"state\": \"running\"}, {\"id\": \"org.example.plain\", \"state\": \"running\"}, "
             "{\"id\": \"org.example.stopped\", \"state\": \"backoff\"}]}", (int)getpid());
    snprintf(path, sizeof(path), "%s/status.json", dir);
    write_file(path, text);
    answers("state", "{\"ok\": true, \"status\": 200, \"body\": {\"title\": \"The exhaust check\", \"done\": true}}");
    json_t *cat = wizpkg_catalog_json();
    char *ct = json_dumps(cat, JSON_COMPACT | JSON_SORT_KEYS);
    CHECK(ct && !strcmp(ct, "[{\"done\":true,\"id\":\"pkg:org.example.check\",\"title\":\"The exhaust check\"}]"),
          "the list: %s", ct ? ct : "none");
    free(ct);
    json_decref(cat);
    CHECK(wizpkg_known("pkg:org.example.check") && !wizpkg_known("pkg:org.example.plain") && !wizpkg_known("pkg:org.example.off")
          && !wizpkg_known("org.example.check") && !wizpkg_known("pkg:../etc"), "which ids are a package's check");
    write_file(path, "{\"pid\": 1, \"services\": [{\"id\": \"org.example.check\", \"state\": \"running\"}]}");
    CHECK(!wizpkg_known("pkg:org.example.check"), "a host that is not alive lists no check");

    char cmd[340];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;
    printf(fails ? "wizpkg_test: %d FAILED\n" : "wizpkg_test: all passed\n", fails);
    return fails ? 1 : 0;
}
