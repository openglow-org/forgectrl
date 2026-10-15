/*
 * wizlive_test.c - host unit test: the live session's exit
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * live_end (wizlive.c) by inclusion, against stubs that record what it
 * does, in order. The rules: the pulse engine settles first; a session
 * that handed out its G92 origin clears it while its controller is up;
 * the lens is stepped over sysfs only with no controller and the engine
 * idle, and a session that cannot put the lens back on its hall edge
 * leaves no marker, so the next controller holds Z unreferenced; every
 * exit hands the posture and the controller back.
 */
#define _GNU_SOURCE
#include "../src/wizlive.c"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

/* What live_end did, in order, and what the stubs answer. */
static char calls[512];
static int stop_rc, settle_rc, rehome_rc;

static void note(const char *what)
{
    size_t n = strlen(calls);
    snprintf(calls + n, sizeof(calls) - n, "%s%s", n ? " " : "", what);
}

/* --- the stubs live_end reaches ------------------------------------- */

int wiz_kernel_settle(double timeout_s) { (void)timeout_s; note("settle"); return settle_rc; }
int wiz_controller_stop(void) { note("stop"); return stop_rc; }
void wiz_controller_start(void) { note("start"); }
int wiz_lens_rehome(void) { note("rehome"); return rehome_rc; }
void lenshome_clear(void) { note("clear"); }
void super_set_local(int on) { note(on ? "local1" : "local0"); }
int wiz_wr_attr(const char *attr, const char *val)
{
    char what[96];
    snprintf(what, sizeof(what), "wr:%s=%s", attr, val);
    note(what);
    return 0;
}
/* The G92 clear: a Grbl socket that answers everything. */
int wiz_grbl_connect(void) { note("g92"); return open("/dev/null", O_RDONLY | O_CLOEXEC); }
int wiz_grbl_send(int fd, const char *line) { (void)fd; (void)line; return 0; }
int wiz_grbl_expect(int fd, const char *until, char *out, size_t olen, int timeout_s)
{
    (void)fd; (void)until; (void)timeout_s;
    if (olen)
        out[0] = '\0';
    return 0;
}
void wiz_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void wiz_phase(const char *fmt, ...) { (void)fmt; }

/* --- the rest of what wizlive.c links against, never reached here --- */

void wiz_progress(int pct) { (void)pct; }
const volatile int *wiz_abort_flag(void) { static volatile int f; return &f; }
const char *wiz_lease_owner(void) { return "wizard:test"; }
void wiz_msleep(int ms) { (void)ms; }
int wiz_wait_for(int (*cond)(void *), void *ctx, int timeout_s)
{
    (void)cond; (void)ctx; (void)timeout_s;
    return -1;
}
int wiz_ask(const char *kind, const char *pid, const char *text, const char *const *opts,
            int nopt, char *answer, size_t alen)
{
    (void)kind; (void)pid; (void)text; (void)opts; (void)nopt; (void)answer; (void)alen;
    return -1;
}
int wiz_ask_t(const char *kind, const char *pid, const char *text, const char *const *opts,
              int nopt, int timeout_s, char *answer, size_t alen)
{
    (void)kind; (void)pid; (void)text; (void)opts; (void)nopt; (void)timeout_s; (void)answer;
    (void)alen;
    return -1;
}
int wiz_ask_continue(const char *pid, const char *text) { (void)pid; (void)text; return -1; }
int wiz_ask_confirm(const char *pid, const char *text) { (void)pid; (void)text; return -1; }
void wiz_wait_open(const char *pid, const char *text, int timeout_s)
{
    (void)pid; (void)text; (void)timeout_s;
}
void wiz_wait_close(void) { }
void wiz_finish_err(const char *fmt, ...) { (void)fmt; }
void wiz_finish_ok(int version, json_t *result, json_t *applied)
{
    (void)version; (void)result; (void)applied;
}
int wiz_write_settings(const char *const *keys, const char *const *vals, size_t n,
                       json_t *applied, char *err, size_t elen)
{
    (void)keys; (void)vals; (void)n; (void)applied;
    if (elen)
        err[0] = '\0';
    return -1;
}
int wiz_imperial(void) { return 0; }
const char *wiz_len(double mm, char *buf, size_t n) { snprintf(buf, n, "%.1f mm", mm); return buf; }
double wiz_len_mm(const char *text) { (void)text; return -1; }
int wiz_kernel_wait_idle(double timeout_s) { (void)timeout_s; return -1; }
int wiz_grbl_up(void *ctx) { (void)ctx; return 0; }
int wiz_z_reference(json_t *r, int passes) { (void)r; (void)passes; return -1; }
int setup_flagged(const char *id) { (void)id; return 0; }
int setup_wizard_version(const char *id) { (void)id; return 0; }
json_t *setup_wizard_result(const char *id) { (void)id; return json_object(); }
int cool_status_json(char *buf, size_t len) { (void)buf; (void)len; return -1; }
double cool_report_age(void) { return -1.0; }
int curverec_override_begin(const char *floor, const char *curve, const char *gamma)
{
    (void)floor; (void)curve; (void)gamma;
    return -1;
}
void curverec_override_end(void) { }
int jobrun_sync(const jobrun_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen)
{
    (void)cfg; (void)run;
    if (elen)
        err[0] = '\0';
    return -1;
}
int led_lit(void) { return 0; }
int grbl_settings_text(char *buf, size_t len) { (void)buf; (void)len; return -1; }
int machine_lid_closed(void) { return 0; }
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }

/* --- the cases -------------------------------------------------------- */

/* One exit: the session as `L` left it, the stubs' answers, and what
 * live_end must do, in order. */
static void exit_does(const char *name, live_t L, int stop, int settle, int rehome,
                      const char *want)
{
    calls[0] = '\0';
    stop_rc = stop;
    settle_rc = settle;
    rehome_rc = rehome;
    live_end(&L);
    char what[640];
    snprintf(what, sizeof(what), "%s: %s", name, calls);
    CHECK(strcmp(calls, want) == 0, what);
    if (strcmp(calls, want))
        printf("     want: %s\n", want);
}

int main(void)
{
    /* A burn stopped after its origin went out: the engine settles, the
     * origin is cleared while the controller is up, the controller is
     * stopped, the lens goes back on its edge, the posture and the
     * controller are handed back. */
    exit_does("a burn stopped after its origin", (live_t){ .local = 1, .g92 = 1 }, 0, 0, 0,
              "settle g92 stop rehome local0 start");
    /* The controller does not leave in the supervisor's grace: the lens
     * is not stepped under it, the marker goes, the machine is handed
     * back all the same. */
    exit_does("the controller did not stop", (live_t){ .local = 1 }, -1, 0, 0,
              "settle stop clear local0 start");
    /* The pulse engine does not settle: the same. */
    exit_does("the engine did not settle", (live_t){ .local = 1 }, 0, -1, 0,
              "settle stop clear local0 start");
    /* A burn played to its tail left the lens on its edge: the marker
     * stands, nothing steps the lens. */
    exit_does("a burn played to its tail", (live_t){ .local = 1, .g92 = 1, .clean = 1 }, 0, 0, 0,
              "settle stop local0 start");
    /* A session that stopped the controller and never started it (the
     * reference failed): the lens goes back on its edge, the controller
     * is started again, the posture never changed. */
    exit_does("stopped before its controller", (live_t){ .stopped = 1 }, 0, 0, 0,
              "settle rehome start");
    /* A hunt that fails leaves the marker to lenshome (it writes none). */
    exit_does("the rehome fails", (live_t){ .local = 1 }, 0, 0, -1,
              "settle stop rehome local0 start");
    /* A session that never stopped the controller touches nothing. */
    exit_does("the stop failed at the start", (live_t){ .local = 0 }, 0, 0, 0, "settle");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
